/*
 * CMPS 342 project (H. Zoueihed), Nov 2026. S5: coordination CMM for B3 / B4 (approved 2026-11-23).
 * Builds one proposal description per command of the cycle from RIC-side information only (intended IMSI from
 * the issuing xApp, cells from the RIC's E2-node registry, cell power from the RIC telemetry store), calls the
 * pure detector / resolver (simulation/xapps/coord-policy.h), and logs every pair classification, rejection and
 * dispatch position in the S3 command lifecycle. Optional fixed coordination hold (CoordinationDelay).
 * Mode: "b3" | "b4-ts" | "b4-tpc" | "b5"; ReverseOrder (B3 sensitivity: TPC list first).
 * b5 (authorised 2026-11-23): dispatch order as B3; every pair flagged by the approved same-cycle detector is
 * resolved by xapp::b5::Decide (b5-policy.h) on the RIC store snapshot at the CMM decision instant; the losing
 * proposal(s) are rejected with reason b5_choice_<X> (or b5_unverifiable), never deferred. Per-decision log B5Log.
 * S5 shadow interaction diagnostic (InteractionLog != ""): labels every TS proposal and TPC reduction under the
 * candidate state-aware / windowed definitions (coord-policy.h, LabelInteractions) and writes them to the log.
 * Labels are never used for resolution: the dispatched commands are identical with and without the log.
 * STATE-AWARE variants (authorised 2026-11-23, decisions 1-2 + "S5 final coordination authorization"):
 *   b4sa-ts / b4sa-tpc  coord-policy.h ResolveStateAwareB4 with SaDefinition (combined | win_2 | win_5)
 *   b5sa                b5sa-policy.h on every proposal flagged by SaDefinition; B5Shadow=true evaluates and logs
 *                       without acting (estimator tests only)
 *   Inputs: RIC store + the CMM's own record of dispatched commands (H) + registered evidence starts. TPC
 *   proposals are judged against the RIC-believed power. Per-proposal log SaLog; B5sa logs B5saLog / B5saPredLog.
 */
#ifndef ORAN_CMM_COORD_H
#define ORAN_CMM_COORD_H

#include "ns3/nstime.h"
#include "ns3/oran-cmm.h"

#include "b5sa-policy.h"
#include "coord-policy.h"

#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace ns3
{

class OranCmmCoord : public OranCmm
{
  public:
    static TypeId GetTypeId();
    OranCmmCoord();
    ~OranCmmCoord() override;
    std::vector<Ptr<OranCommand>> Filter(
        std::map<std::tuple<std::string, bool>, std::vector<Ptr<OranCommand>>> inputCommands) override;

  private:
    bool StateAware() const;
    xapp::coord::LabelInput MakeLabelInput(const std::vector<xapp::coord::Prop>& props) const;
    void StateAwareResolve(const std::vector<Ptr<OranCommand>>& cmds, const std::vector<xapp::coord::Prop>& sp,
                           std::set<size_t>& drop, std::vector<std::pair<size_t, std::string>>& rej);
    std::vector<xapp::b5sa::Scenario> BuildScenarios(double now, const std::set<uint16_t>& cells, double tpcRequested,
                                                     uint16_t tpcCell, std::string& block, std::string& desc,
                                                     double& newest, int& nUes, int& nUnver) const;
    std::string m_saDef;
    std::string m_saLog, m_b5saLog, m_b5saPredLog;
    std::ofstream m_sa, m_b5sa, m_b5saPred;
    bool m_b5Shadow;
    uint32_t m_maxUncertain;
    uint64_t m_b5saN{0};
    void Dispatch(std::vector<Ptr<OranCommand>> commands);
    void Record(const std::vector<Ptr<OranCommand>>& dispatched);
    std::string B5Resolve(const xapp::coord::Prop& ts, const xapp::coord::Prop& tpc);
    std::string m_b5Log;
    std::ofstream m_b5;
    std::string m_lastB5Reason, m_lastB5Detail;
    void Diagnose(const std::vector<Ptr<OranCommand>>& cmds, const std::vector<xapp::coord::Prop>& props);
    std::string m_mode;
    std::string m_interactionLog;
    double m_diagPendingS;
    std::ofstream m_diag;
    std::vector<xapp::coord::HistCmd> m_hist; //!< RIC-side record of dispatched commands
    std::map<uint64_t, xapp::coord::Prop> m_propOf; //!< cmd id -> its proposal description
    bool m_reverse;
    double m_nominalDbm;
    Time m_coordinationDelay;
};

} // namespace ns3

#endif
