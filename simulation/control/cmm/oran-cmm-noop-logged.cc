/*
 * CMPS 342 project (H. Zoueihed), Nov 2026. S3 (see header).
 */
#include "oran-cmm-noop-logged.h"

#include "cmd-lifecycle.h"

#include "ns3/log.h"
#include "ns3/oran-near-rt-ric-e2terminator.h"
#include "ns3/oran-near-rt-ric.h"
#include "ns3/simulator.h"

namespace ns3
{

NS_LOG_COMPONENT_DEFINE("OranCmmNoopLogged");
NS_OBJECT_ENSURE_REGISTERED(OranCmmNoopLogged);

TypeId
OranCmmNoopLogged::GetTypeId()
{
    static TypeId tid =
        TypeId("ns3::OranCmmNoopLogged")
            .SetParent<OranCmmNoop>()
            .AddConstructor<OranCmmNoopLogged>()
            .AddAttribute("CoordinationDelay",
                          "Fixed hold between CMM input and dispatch (0 = none).",
                          TimeValue(Seconds(0)),
                          MakeTimeAccessor(&OranCmmNoopLogged::m_coordinationDelay),
                          MakeTimeChecker());
    return tid;
}

OranCmmNoopLogged::OranCmmNoopLogged()
    : OranCmmNoop()
{
}

OranCmmNoopLogged::~OranCmmNoopLogged() = default;

std::vector<Ptr<OranCommand>>
OranCmmNoopLogged::Filter(
    std::map<std::tuple<std::string, bool>, std::vector<Ptr<OranCommand>>> inputCommands)
{
    std::vector<Ptr<OranCommand>> out = OranCmmNoop::Filter(inputCommands);
    if (xapp::cmdlife::IsOpen())
    {
        for (const auto& kv : inputCommands)
        {
            for (const auto& c : kv.second)
            {
                xapp::cmdlife::Stage(c, "cmm_in", "", "", "from " + std::get<0>(kv.first));
            }
        }
    }
    if (m_coordinationDelay > Seconds(0))
    {
        Simulator::Schedule(m_coordinationDelay, &OranCmmNoopLogged::Dispatch, this, out);
        return {};
    }
    // The RIC passes the returned list to its E2 terminator at this same instant.
    if (xapp::cmdlife::IsOpen())
    {
        size_t k = 0;
        for (const auto& c : out)
        {
            xapp::cmdlife::Stage(c, "dispatched", "", "", "list position " + std::to_string(k++));
        }
    }
    return out;
}

void
OranCmmNoopLogged::Dispatch(std::vector<Ptr<OranCommand>> commands)
{
    size_t k = 0;
    for (const auto& c : commands)
    {
        xapp::cmdlife::Stage(c, "dispatched", "", "", "list position " + std::to_string(k++) +
                                                          " after coordination hold");
    }
    m_nearRtRic->GetE2Terminator()->ProcessCommands(commands);
}

} // namespace ns3
