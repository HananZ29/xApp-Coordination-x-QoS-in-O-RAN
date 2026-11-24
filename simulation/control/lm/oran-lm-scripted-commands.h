/*
 * CMPS 342 project (H. Zoueihed), Nov 2026. S3 TEST FIXTURE, not an xApp.
 *
 * Issues a fixed script of handover / transmit-power commands so that the actuators, the validity
 * rules V1/V2 and the timing pipeline can be checked against known answers. It reads ground truth
 * (UE RNTI and serving cell) from the simulator instead of E2 reports; it contains no decision logic.
 *
 * Script: entries separated by ';', fields by ':'
 *   <t>:ho:ue=<index>:cell=<target>[:rnti=<r>][:src=<cell>]   handover of UE <index>
 *   <t>:tpc:cell=<c>:dbm=<value>                               transmit power (value may be nan)
 * Each entry is issued once, at the first RIC query with Now >= t, in script order.
 * rnti / src override the true RNTI / source cell (invalid-command tests).
 * Optional :ev=<t> (S5 state-aware tests): registers <t> as the command's evidence start at the RIC, as the TS / TPC
 * xApps do for their own proposals (coord-policy.h, 'evidence'). Without it the evidence start is unknown (= now).
 */
#ifndef ORAN_LM_SCRIPTED_COMMANDS_H
#define ORAN_LM_SCRIPTED_COMMANDS_H

#include "ns3/nstime.h"
#include "ns3/oran-lm.h"

#include <string>
#include <vector>

namespace ns3
{

class OranLmScriptedCommands : public OranLm
{
  public:
    static TypeId GetTypeId();
    OranLmScriptedCommands();
    ~OranLmScriptedCommands() override;

  protected:
    std::vector<Ptr<OranCommand>> Run() override;
    void FinishRun() override;

  private:
    struct Entry
    {
        double t{0};
        std::string kind;
        int ue{-1};
        int cell{-1};
        int rnti{-1};
        int src{-1};
        double dbm{0};
        double ev{-1e300};
        bool issued{false};
        std::string text;
    };
    void Parse();
    std::string m_script;
    std::vector<Entry> m_entries;
    bool m_parsed{false};
    Time m_runCycle;                            //!< t_q of the current run
    std::vector<Ptr<OranCommand>> m_inFlight;   //!< created, LM output not yet delivered
    uint64_t m_nextRequestId{1};
};

} // namespace ns3

#endif
