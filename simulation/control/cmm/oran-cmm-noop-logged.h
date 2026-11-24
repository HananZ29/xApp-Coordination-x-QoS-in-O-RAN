/*
 * CMPS 342 project (H. Zoueihed), Nov 2026. S3.
 * OranCmmNoop (NIST) with per-command lifecycle logging and an optional fixed hold before
 * dispatch (CoordinationDelay, default 0 = identical timing to OranCmmNoop). No filtering,
 * reordering or arbitration: the output list is exactly OranCmmNoop's.
 */
#ifndef ORAN_CMM_NOOP_LOGGED_H
#define ORAN_CMM_NOOP_LOGGED_H

#include "ns3/nstime.h"
#include "ns3/oran-cmm-noop.h"

namespace ns3
{

class OranCmmNoopLogged : public OranCmmNoop
{
  public:
    static TypeId GetTypeId();
    OranCmmNoopLogged();
    ~OranCmmNoopLogged() override;
    std::vector<Ptr<OranCommand>> Filter(
        std::map<std::tuple<std::string, bool>, std::vector<Ptr<OranCommand>>> inputCommands)
        override;

  private:
    void Dispatch(std::vector<Ptr<OranCommand>> commands);
    Time m_coordinationDelay;
};

} // namespace ns3

#endif
