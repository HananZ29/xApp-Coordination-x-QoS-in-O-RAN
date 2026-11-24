/*
 * CMPS 342 project (H. Zoueihed), Nov 2026. S4: RIC-side telemetry store (see header).
 */
#include "ric-telemetry-store.h"

#include "oran-report-lte-enb-load.h"
#include "oran-report-lte-ue-service.h"

#include "ns3/oran-report-lte-ue-rsrp-rsrq.h"

#include <cmath>
#include <deque>
#include <fstream>
#include <iomanip>
#include <map>
#include <optional>
#include <tuple>

using namespace ns3;

namespace xapp
{
namespace ric
{
namespace
{
const double PAY = 1024.0 / 1054.0; // application payload / PDCP PDU (1024-B UDP payload); protocol constant

struct Rsrp
{
    double t{0};
    double dbm{0};
    bool serving{false};
    uint16_t rnti{0};
};

std::map<uint16_t, std::deque<policy::LoadRep>> g_load;
std::map<uint64_t, policy::SvcRep> g_svc;                   // imsi -> latest
std::map<std::pair<uint64_t, uint16_t>, std::deque<std::pair<double, double>>> g_dem; // (imsi, cell) -> last 4 (t0, demand) (I3b)
double g_tA = -1e18;                                         // C-A admission start (-inf: off)
std::map<uint64_t, double> g_evidence;                       // cmd id -> evidence start
std::map<std::pair<uint16_t, uint16_t>, uint64_t> g_ident; // (cell, rnti) -> imsi (latest service report)
std::map<uint64_t, std::map<uint16_t, Rsrp>> g_rsrp;        // UE E2 node -> cell -> latest
std::ofstream g_dec;
} // namespace

void
OnReport(Ptr<OranReport> report)
{
    if (auto r = DynamicCast<OranReportLteEnbLoad>(report))
    {
        policy::LoadRep l;
        l.t = r->GetTime().GetSeconds();
        l.t0 = r->GetIntervalStart().GetSeconds();
        l.prb = r->GetPrbUtilSched();
        l.holMs = r->GetMaxHolMs();
        l.dropBytes = r->GetRlcDropBytes();
        l.txPowerDbm = r->GetTxPowerDbm();
        auto& q = g_load[r->GetCellId()];
        q.push_front(l);
        while (q.size() > 8)
        {
            q.pop_back();
        }
    }
    else if (auto r = DynamicCast<OranReportLteUeService>(report))
    {
        policy::SvcRep s;
        s.t = r->GetTime().GetSeconds();
        s.t0 = r->GetIntervalStart().GetSeconds();
        s.cell = r->GetCellId();
        s.rnti = r->GetRnti();
        double dt = s.t - s.t0;
        s.servedMbps = dt > 0 ? r->GetOutBytes() * 8.0 / dt / 1e6 * PAY : 0;
        s.demandMbps = dt > 0 ? r->GetInBytes() * 8.0 / dt / 1e6 * PAY : 0;
        uint64_t al = r->GetAckedBytes() + r->GetHarqLostBytes();
        s.harqLossShare = al > 0 ? double(r->GetHarqLostBytes()) / al : 0;
        s.holMs = r->GetHolNowMs();
        auto& dq = g_dem[{r->GetImsi(), s.cell}];
        dq.emplace_back(s.t0, s.demandMbps);
        while (dq.size() > 4)
        {
            dq.pop_front();
        }
        double sum = 0;
        for (const auto& [t0, v] : dq)
        {
            sum += v;
        }
        s.demandMeanMbps = sum / dq.size();
        auto it = g_svc.find(r->GetImsi());
        if (it == g_svc.end() || it->second.t <= s.t)
        {
            g_svc[r->GetImsi()] = s;
        }
        g_ident[{s.cell, s.rnti}] = r->GetImsi();
    }
    else if (auto r = DynamicCast<OranReportLteUeRsrpRsrq>(report))
    {
        Rsrp x;
        x.t = r->GetTime().GetSeconds();
        x.dbm = r->GetRsrp();
        x.serving = r->GetIsServingCell();
        x.rnti = r->GetRnti();
        auto& m = g_rsrp[r->GetReporterE2NodeId()];
        auto it = m.find(r->GetCellId());
        if (it == m.end() || it->second.t <= x.t)
        {
            m[r->GetCellId()] = x;
        }
    }
}

policy::Snapshot
BuildSnapshot(double now)
{
    policy::Snapshot s;
    s.now = now;
    const bool ca = g_tA > -1e17;
    for (const auto& [c, q] : g_load)
    {
        if (!ca)
        {
            s.load[c] = std::vector<policy::LoadRep>(q.begin(), q.end());
            continue;
        }
        std::vector<policy::LoadRep> v;
        for (const auto& l : q)
        {
            if (l.t0 >= g_tA - 1e-9)
            {
                v.push_back(l);
            }
        }
        if (!v.empty())
        {
            s.load[c] = v;
        }
    }
    // C-A: the latest service report is visible only if its window is admissible; its I3b demand mean is
    // recomputed over the admissible terms only
    auto admitSvc = [&](uint64_t imsi, const policy::SvcRep& sv) -> std::optional<policy::SvcRep> {
        if (!ca)
        {
            return sv;
        }
        if (sv.t0 < g_tA - 1e-9)
        {
            return std::nullopt;
        }
        policy::SvcRep r = sv;
        auto it = g_dem.find({imsi, sv.cell});
        double sum = 0;
        int n = 0;
        if (it != g_dem.end())
        {
            for (const auto& [t0, v] : it->second)
            {
                if (t0 >= g_tA - 1e-9)
                {
                    sum += v;
                    n++;
                }
            }
        }
        r.demandMeanMbps = n > 0 ? sum / n : r.demandMbps;
        return r;
    };
    for (const auto& [node, m] : g_rsrp)
    {
        policy::UeView u;
        double tServ = -1;
        for (const auto& [cell, x] : m)
        {
            u.rsrpDbm[cell] = x.dbm;
            u.rsrpAge[cell] = now - x.t;
            if (x.serving && x.t > tServ)
            {
                tServ = x.t;
                u.servingCell = cell;
                u.rnti = x.rnti;
            }
        }
        if (tServ < 0)
        {
            continue;
        }
        u.servingAge = now - tServ;
        auto id = g_ident.find({u.servingCell, u.rnti});
        if (id != g_ident.end())
        {
            u.imsi = id->second;
            auto sv = g_svc.find(u.imsi);
            if (sv != g_svc.end())
            {
                u.svc = admitSvc(u.imsi, sv->second);
            }
        }
        s.ues.push_back(u);
    }
    // UEs reported by an eNB but without (fresh) UE measurement reports: visible to the controllers as
    // served by that cell with no radio information (TPC treats them as unavailable; TS cannot steer them)
    std::map<uint64_t, bool> seen;
    for (const auto& u : s.ues)
    {
        seen[u.imsi] = true;
    }
    for (const auto& [imsi, sv] : g_svc)
    {
        if (seen.count(imsi))
        {
            continue;
        }
        auto adm = admitSvc(imsi, sv);
        if (!adm)
        {
            continue;
        }
        policy::UeView u;
        u.imsi = imsi;
        u.servingCell = sv.cell;
        u.rnti = sv.rnti;
        u.servingAge = now - sv.t;
        u.svc = adm;
        s.ues.push_back(u);
    }
    return s;
}

void
SetAdmissionStart(double tA)
{
    g_tA = tA;
}

double
AdmissionStart()
{
    return g_tA;
}

int
AdmissibleReports()
{
    int n = 0;
    for (const auto& [c, q] : g_load)
    {
        for (const auto& l : q)
        {
            n += l.t0 >= g_tA - 1e-9;
        }
    }
    for (const auto& [imsi, sv] : g_svc)
    {
        n += sv.t0 >= g_tA - 1e-9;
    }
    return n;
}

void
RegisterEvidence(uint64_t cmdId, double evidenceStartS)
{
    g_evidence[cmdId] = evidenceStartS;
}

double
EvidenceStart(uint64_t cmdId)
{
    auto it = g_evidence.find(cmdId);
    return it == g_evidence.end() ? std::nan("") : it->second;
}

std::pair<uint16_t, double>
LatestServiceObservation(uint64_t imsi)
{
    auto it = g_svc.find(imsi);
    if (it == g_svc.end())
    {
        return {0, -1e18};
    }
    return {it->second.cell, it->second.t};
}

double
LatestLoadReportTime(uint16_t cell)
{
    auto it = g_load.find(cell);
    return it == g_load.end() || it->second.empty() ? -1e18 : it->second.front().t;
}

double
LatestTxPowerDbm(uint16_t cell)
{
    auto it = g_load.find(cell);
    if (it == g_load.end() || it->second.empty() || !(it->second.front().txPowerDbm > -1e8))
    {
        return std::nan("");
    }
    return it->second.front().txPowerDbm;
}

void
OpenDecisionLog(const std::string& outPrefix)
{
    g_dec.open(outPrefix + "-xappdec.csv");
    g_dec << "time_s,xapp,entity,decision,reason,detail\n";
}

void
LogDecisions(double now, const std::vector<policy::LogEntry>& entries)
{
    if (!g_dec.is_open())
    {
        return;
    }
    for (const auto& e : entries)
    {
        std::string d = e.detail;
        for (auto& ch : d)
        {
            if (ch == ',')
                ch = ';';
        }
        g_dec << std::fixed << std::setprecision(6) << now << "," << e.xapp << "," << e.entity << ","
              << e.decision << "," << e.reason << "," << d << "\n";
    }
}

void
CloseDecisionLog()
{
    if (g_dec.is_open())
    {
        g_dec.close();
    }
}

} // namespace ric
} // namespace xapp
