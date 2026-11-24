/*
 * CMPS 342 project (H. Zoueihed), Nov 2026. Stage S2: modelled eNB load telemetry.
 */

#include "oran-report-lte-enb-load.h"

#include "ns3/double.h"
#include "ns3/log.h"
#include "ns3/uinteger.h"

#include <cmath>
#include <sstream>

namespace ns3
{

NS_LOG_COMPONENT_DEFINE("OranReportLteEnbLoad");
NS_OBJECT_ENSURE_REGISTERED(OranReportLteEnbLoad);

TypeId
OranReportLteEnbLoad::GetTypeId()
{
    static TypeId tid =
        TypeId("ns3::OranReportLteEnbLoad")
            .SetParent<OranReport>()
            .AddConstructor<OranReportLteEnbLoad>()
            .AddAttribute("CellId", "Reporting cell.", UintegerValue(0),
                          MakeUintegerAccessor(&OranReportLteEnbLoad::m_cellId),
                          MakeUintegerChecker<uint16_t>())
            .AddAttribute("IntervalStart", "Start of the covered interval.", TimeValue(Seconds(0)),
                          MakeTimeAccessor(&OranReportLteEnbLoad::m_intervalStart),
                          MakeTimeChecker())
            .AddAttribute("DataRbSum", "DL data RBs radiated in the interval.", UintegerValue(0),
                          MakeUintegerAccessor(&OranReportLteEnbLoad::m_dataRbSum),
                          MakeUintegerChecker<uint64_t>())
            .AddAttribute("Subframes", "1 ms subframes in the interval.", UintegerValue(0),
                          MakeUintegerAccessor(&OranReportLteEnbLoad::m_subframes),
                          MakeUintegerChecker<uint64_t>())
            .AddAttribute("SchedulableRbs", "Schedulable RBs per subframe.", UintegerValue(24),
                          MakeUintegerAccessor(&OranReportLteEnbLoad::m_nRbSched),
                          MakeUintegerChecker<uint32_t>())
            .AddAttribute("PhysicalRbs", "Physical RBs per subframe.", UintegerValue(25),
                          MakeUintegerAccessor(&OranReportLteEnbLoad::m_nRbPhys),
                          MakeUintegerChecker<uint32_t>())
            .AddAttribute("BacklogMeanBytes", "Time-mean summed DRB RLC queue over the interval.",
                          DoubleValue(0),
                          MakeDoubleAccessor(&OranReportLteEnbLoad::m_backlogMeanBytes),
                          MakeDoubleChecker<double>())
            .AddAttribute("BacklogNowBytes", "Summed DRB RLC queue at report time.",
                          UintegerValue(0),
                          MakeUintegerAccessor(&OranReportLteEnbLoad::m_backlogNowBytes),
                          MakeUintegerChecker<uint64_t>())
            .AddAttribute("MaxHolMs", "Max DRB head-of-line delay at report time (ms).",
                          UintegerValue(0), MakeUintegerAccessor(&OranReportLteEnbLoad::m_maxHolMs),
                          MakeUintegerChecker<uint32_t>())
            .AddAttribute("RlcDropBytes", "RLC transmit-buffer drops in the interval.",
                          UintegerValue(0),
                          MakeUintegerAccessor(&OranReportLteEnbLoad::m_rlcDropBytes),
                          MakeUintegerChecker<uint64_t>())
            .AddAttribute("TxPowerDbm", "S4: cell DL transmit power at report time (NaN = not reported).",
                          DoubleValue(std::nan("")),
                          MakeDoubleAccessor(&OranReportLteEnbLoad::m_txPowerDbm),
                          MakeDoubleChecker<double>());
    return tid;
}

OranReportLteEnbLoad::OranReportLteEnbLoad()
    : OranReport()
{
}

OranReportLteEnbLoad::~OranReportLteEnbLoad() = default;

std::string
OranReportLteEnbLoad::ToString() const
{
    std::ostringstream ss;
    ss << "cell=" << m_cellId << ";interval_start_s=" << m_intervalStart.GetSeconds()
       << ";subframes=" << m_subframes << ";data_rb_sum=" << m_dataRbSum
       << ";prb_util_sched=" << GetPrbUtilSched() << ";prb_util_phys=" << GetPrbUtilPhys()
       << ";backlog_mean_bytes=" << m_backlogMeanBytes << ";backlog_now_bytes=" << m_backlogNowBytes
       << ";max_hol_ms=" << m_maxHolMs << ";rlc_drop_bytes=" << m_rlcDropBytes;
    if (!std::isnan(m_txPowerDbm))
    {
        ss << ";tx_power_dbm=" << m_txPowerDbm; // only when enabled (S4); legacy payload unchanged
    }
    return ss.str();
}

double
OranReportLteEnbLoad::GetTxPowerDbm() const
{
    return m_txPowerDbm;
}

uint16_t
OranReportLteEnbLoad::GetCellId() const
{
    return m_cellId;
}

Time
OranReportLteEnbLoad::GetIntervalStart() const
{
    return m_intervalStart;
}

uint64_t
OranReportLteEnbLoad::GetDataRbSum() const
{
    return m_dataRbSum;
}

uint64_t
OranReportLteEnbLoad::GetSubframes() const
{
    return m_subframes;
}

double
OranReportLteEnbLoad::GetPrbUtilSched() const
{
    return m_subframes ? double(m_dataRbSum) / (double(m_nRbSched) * m_subframes) : 0.0;
}

double
OranReportLteEnbLoad::GetPrbUtilPhys() const
{
    return m_subframes ? double(m_dataRbSum) / (double(m_nRbPhys) * m_subframes) : 0.0;
}

double
OranReportLteEnbLoad::GetBacklogMeanBytes() const
{
    return m_backlogMeanBytes;
}

uint64_t
OranReportLteEnbLoad::GetBacklogNowBytes() const
{
    return m_backlogNowBytes;
}

uint32_t
OranReportLteEnbLoad::GetMaxHolMs() const
{
    return m_maxHolMs;
}

uint64_t
OranReportLteEnbLoad::GetRlcDropBytes() const
{
    return m_rlcDropBytes;
}

} // namespace ns3
