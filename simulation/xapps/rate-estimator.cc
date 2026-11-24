/*
 * CMPS 342 project (H. Zoueihed), Nov 2026. S5: telemetry-only per-UE rate estimator (see header).
 */
#include "rate-estimator.h"

#include "ns3/lte-amc.h"
#include "ns3/lte-spectrum-value-helper.h"
#include "ns3/spectrum-value.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>

namespace xapp
{
namespace est
{

double
AmcLut::Eta(double gDb) const
{
    if (bytesPerRb.empty() || std::isnan(gDb))
    {
        return 0.0;
    }
    double k = std::floor((gDb - gMinDb) / stepDb + 1e-9);
    if (k < 0)
    {
        return 0.0;
    }
    size_t i = std::min(static_cast<size_t>(k), bytesPerRb.size() - 1);
    return bytesPerRb[i];
}

AmcLut
BuildAmcLut(int nRbCfg, int nRbSched, double payloadFraction, double gMinDb, double gMaxDb, double stepDb)
{
    AmcLut t;
    t.gMinDb = gMinDb;
    t.stepDb = stepDb;
    ns3::Ptr<ns3::LteAmc> amc = ns3::CreateObject<ns3::LteAmc>(); // default attributes, as the UE PHY / scheduler
    ns3::Ptr<const ns3::SpectrumModel> sm = ns3::LteSpectrumValueHelper::GetSpectrumModel(100, nRbCfg);
    const int rbg = nRbCfg < 10 ? 1 : nRbCfg < 26 ? 2 : nRbCfg < 63 ? 3 : 4; // PF Type-0 RBG size
    int n = static_cast<int>(std::lround((gMaxDb - gMinDb) / stepDb)) + 1;
    for (int k = 0; k < n; k++)
    {
        double g = gMinDb + k * stepDb;
        ns3::SpectrumValue sinr(sm);
        sinr = std::pow(10.0, g / 10.0);
        std::vector<int> cq = amc->CreateCqiFeedbacks(sinr, rbg);
        int c = cq.empty() ? 0 : std::max(0, cq.front());
        int m = c > 0 ? amc->GetMcsFromCqi(c) : -1;
        double bytes = m >= 0 ? amc->GetDlTbSizeFromMcs(m, nRbSched) / 8.0 / nRbSched * payloadFraction : 0.0;
        t.cqi.push_back(c);
        t.mcs.push_back(m);
        t.bytesPerRb.push_back(bytes);
    }
    return t;
}

Output
Predict(const Input& in, const Candidate& a, const AmcLut& lut)
{
    Output o;
    const size_t n = in.ues.size();
    std::vector<uint16_t> cellAfter(n);
    std::set<uint16_t> cells;
    for (const auto& [c, r] : in.rhoInit)
    {
        cells.insert(c);
    }
    for (size_t i = 0; i < n; i++)
    {
        cellAfter[i] = (a.moveImsi != 0 && in.ues[i].imsi == a.moveImsi) ? a.moveTo : in.ues[i].cell;
        cells.insert(cellAfter[i]);
        cells.insert(in.ues[i].cell);
    }
    std::map<uint16_t, double> rho;
    for (uint16_t c : cells)
    {
        auto it = in.rhoInit.find(c);
        rho[c] = it == in.rhoInit.end() ? 1.0 : std::clamp(it->second, 0.0, 1.0);
    }
    auto dP = [&](uint16_t c) {
        auto it = a.deltaDb.find(c);
        return it == a.deltaDb.end() ? 0.0 : it->second;
    };
    const double noise = std::pow(10.0, in.nReDbm / 10.0);
    std::vector<UeOut> ue(n);
    std::map<uint16_t, double> load;
    for (int it = 1; it <= std::max(1, in.maxIter); it++)
    {
        o.iterations = it;
        load.clear();
        for (size_t i = 0; i < n; i++)
        {
            const UeIn& u = in.ues[i];
            UeOut& r = ue[i];
            r.imsi = u.imsi;
            r.cellAfter = cellAfter[i];
            auto s = u.rsrpDbm.find(cellAfter[i]);
            double den = noise;
            for (const auto& [j, p] : u.rsrpDbm)
            {
                if (j != cellAfter[i])
                {
                    den += rho[j] * std::pow(10.0, (p + dP(j)) / 10.0);
                }
            }
            r.gammaDb = s == u.rsrpDbm.end() ? -std::numeric_limits<double>::infinity()
                                             : s->second + dP(cellAfter[i]) - 10.0 * std::log10(den);
            r.etaBytes = in.kappa * lut.Eta(r.gammaDb);
            r.capMbps = in.nRbSched * r.etaBytes * 1000.0 * 8.0 / 1e6;
            r.rbTime = u.demandMbps <= 0 ? 0.0
                       : r.capMbps > 0   ? u.demandMbps / r.capMbps
                                         : std::numeric_limits<double>::infinity();
            load[cellAfter[i]] += r.rbTime;
        }
        // time shares per cell: PF water-filling of equal shares when overloaded
        for (uint16_t c : cells)
        {
            std::vector<size_t> idx;
            for (size_t i = 0; i < n; i++)
            {
                if (cellAfter[i] == c)
                {
                    idx.push_back(i);
                }
            }
            if (load[c] <= 1.0)
            {
                for (size_t i : idx)
                {
                    ue[i].tau = ue[i].rbTime;
                }
                continue;
            }
            std::sort(idx.begin(), idx.end(), [&](size_t x, size_t y) {
                return ue[x].rbTime != ue[y].rbTime ? ue[x].rbTime < ue[y].rbTime : in.ues[x].imsi < in.ues[y].imsi;
            });
            double left = 1.0;
            size_t k = idx.size();
            for (size_t m = 0; m < idx.size(); m++, k--)
            {
                double share = left / k;
                double take = std::min(ue[idx[m]].rbTime, share);
                ue[idx[m]].tau = take;
                left -= take;
            }
        }
        double dmax = 0;
        std::map<uint16_t, double> nrho;
        for (uint16_t c : cells)
        {
            nrho[c] = std::min(1.0, load[c]);
            dmax = std::max(dmax, std::fabs(nrho[c] - rho[c]));
        }
        rho = nrho;
        if (dmax < in.tol)
        {
            o.converged = true;
            break;
        }
    }
    for (size_t i = 0; i < n; i++)
    {
        ue[i].rateMbps = std::min(in.ues[i].demandMbps, ue[i].tau * ue[i].capMbps);
    }
    o.ues = ue;
    o.rho = rho;
    o.load = load;
    return o;
}

} // namespace est
} // namespace xapp
