/*
 * CMPS 342 project (H. Zoueihed), Nov 2026. See link-shadowing-loss-model.h.
 */

#include "link-shadowing-loss-model.h"

#include "ns3/double.h"
#include "ns3/log.h"
#include "ns3/mobility-model.h"
#include "ns3/node.h"

#include <algorithm>
#include <iomanip>

namespace ns3
{

NS_LOG_COMPONENT_DEFINE("LinkShadowingLossModel");
NS_OBJECT_ENSURE_REGISTERED(LinkShadowingLossModel);

TypeId
LinkShadowingLossModel::GetTypeId()
{
    static TypeId tid = TypeId("ns3::LinkShadowingLossModel")
                            .SetParent<PropagationLossModel>()
                            .AddConstructor<LinkShadowingLossModel>()
                            .AddAttribute("SigmaDb",
                                          "Log-normal shadowing standard deviation (dB); TR 38.901 "
                                          "UMa NLOS: 6 dB.",
                                          DoubleValue(6.0),
                                          MakeDoubleAccessor(&LinkShadowingLossModel::m_sigmaDb),
                                          MakeDoubleChecker<double>(0.0));
    return tid;
}

LinkShadowingLossModel::LinkShadowingLossModel()
{
    m_normal = CreateObject<NormalRandomVariable>();
    m_normal->SetAttribute("Mean", DoubleValue(0.0));
    m_normal->SetAttribute("Variance", DoubleValue(1.0));
}

LinkShadowingLossModel::~LinkShadowingLossModel() = default;

double
LinkShadowingLossModel::DoCalcRxPower(double txPowerDbm, Ptr<MobilityModel> a, Ptr<MobilityModel> b) const
{
    uint32_t ia = a->GetObject<Node>()->GetId();
    uint32_t ib = b->GetObject<Node>()->GetId();
    std::pair<uint32_t, uint32_t> key{std::min(ia, ib), std::max(ia, ib)};
    auto it = m_cache.find(key);
    if (it == m_cache.end())
    {
        it = m_cache.emplace(key, m_normal->GetValue() * m_sigmaDb).first;
    }
    return txPowerDbm - it->second;
}

int64_t
LinkShadowingLossModel::DoAssignStreams(int64_t stream)
{
    m_normal->SetStream(stream);
    return 1;
}

void
LinkShadowingLossModel::Dump(std::ostream& os) const
{
    os << "node_a,node_b,shadowing_db\n";
    for (const auto& [k, v] : m_cache)
    {
        os << k.first << "," << k.second << "," << std::fixed << std::setprecision(6) << v << "\n";
    }
}

} // namespace ns3
