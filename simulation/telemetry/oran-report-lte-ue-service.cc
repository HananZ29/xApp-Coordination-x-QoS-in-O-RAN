/*
 * CMPS 342 project (H. Zoueihed), Nov 2026. Stage S4 preparation: per-UE DL service telemetry.
 */
#include "oran-report-lte-ue-service.h"

#include "ns3/uinteger.h"

#include <sstream>

namespace ns3
{

NS_OBJECT_ENSURE_REGISTERED(OranReportLteUeService);

TypeId
OranReportLteUeService::GetTypeId()
{
    static TypeId tid =
        TypeId("ns3::OranReportLteUeService")
            .SetParent<OranReport>()
            .AddConstructor<OranReportLteUeService>()
            .AddAttribute("CellId", "Reporting cell.", UintegerValue(0),
                          MakeUintegerAccessor(&OranReportLteUeService::m_cellId),
                          MakeUintegerChecker<uint16_t>())
            .AddAttribute("Rnti", "UE RNTI in the reporting cell.", UintegerValue(0),
                          MakeUintegerAccessor(&OranReportLteUeService::m_rnti),
                          MakeUintegerChecker<uint16_t>())
            .AddAttribute("Imsi", "UE IMSI (from the eNB UE context).", UintegerValue(0),
                          MakeUintegerAccessor(&OranReportLteUeService::m_imsi),
                          MakeUintegerChecker<uint64_t>())
            .AddAttribute("IntervalStart", "Start of the covered interval.", TimeValue(Seconds(0)),
                          MakeTimeAccessor(&OranReportLteUeService::m_intervalStart),
                          MakeTimeChecker())
            .AddAttribute("InBytes", "PDCP PDU bytes arrived at RLC in the interval.", UintegerValue(0),
                          MakeUintegerAccessor(&OranReportLteUeService::m_inBytes),
                          MakeUintegerChecker<uint64_t>())
            .AddAttribute("OutBytes", "RLC payload bytes handed to MAC in the interval.", UintegerValue(0),
                          MakeUintegerAccessor(&OranReportLteUeService::m_outBytes),
                          MakeUintegerChecker<uint64_t>())
            .AddAttribute("DropBytes", "RLC transmit-buffer drops in the interval.", UintegerValue(0),
                          MakeUintegerAccessor(&OranReportLteUeService::m_dropBytes),
                          MakeUintegerChecker<uint64_t>())
            .AddAttribute("AckedBytes", "RLC payload in HARQ-ACKed DL TBs in the interval.", UintegerValue(0),
                          MakeUintegerAccessor(&OranReportLteUeService::m_ackedBytes),
                          MakeUintegerChecker<uint64_t>())
            .AddAttribute("HarqLostBytes", "RLC payload in DL TBs abandoned without ACK.", UintegerValue(0),
                          MakeUintegerAccessor(&OranReportLteUeService::m_harqLostBytes),
                          MakeUintegerChecker<uint64_t>())
            .AddAttribute("BacklogNowBytes", "RLC queue at report time.", UintegerValue(0),
                          MakeUintegerAccessor(&OranReportLteUeService::m_backlogNowBytes),
                          MakeUintegerChecker<uint64_t>())
            .AddAttribute("HolNowMs", "Head-of-line delay at report time (ms).", UintegerValue(0),
                          MakeUintegerAccessor(&OranReportLteUeService::m_holNowMs),
                          MakeUintegerChecker<uint32_t>());
    return tid;
}

OranReportLteUeService::OranReportLteUeService() = default;
OranReportLteUeService::~OranReportLteUeService() = default;

std::string
OranReportLteUeService::ToString() const
{
    std::ostringstream s;
    s << "cell=" << m_cellId << ";rnti=" << m_rnti << ";imsi=" << m_imsi
      << ";t0=" << m_intervalStart.GetSeconds() << ";in=" << m_inBytes << ";out=" << m_outBytes
      << ";drop=" << m_dropBytes << ";acked=" << m_ackedBytes << ";harqlost=" << m_harqLostBytes << ";backlog=" << m_backlogNowBytes << ";hol=" << m_holNowMs;
    return s.str();
}

uint16_t OranReportLteUeService::GetCellId() const { return m_cellId; }
uint16_t OranReportLteUeService::GetRnti() const { return m_rnti; }
uint64_t OranReportLteUeService::GetImsi() const { return m_imsi; }
Time OranReportLteUeService::GetIntervalStart() const { return m_intervalStart; }
uint64_t OranReportLteUeService::GetInBytes() const { return m_inBytes; }
uint64_t OranReportLteUeService::GetOutBytes() const { return m_outBytes; }
uint64_t OranReportLteUeService::GetDropBytes() const { return m_dropBytes; }
uint64_t OranReportLteUeService::GetAckedBytes() const { return m_ackedBytes; }
uint64_t OranReportLteUeService::GetHarqLostBytes() const { return m_harqLostBytes; }
uint64_t OranReportLteUeService::GetBacklogNowBytes() const { return m_backlogNowBytes; }
uint32_t OranReportLteUeService::GetHolNowMs() const { return m_holNowMs; }

} // namespace ns3
