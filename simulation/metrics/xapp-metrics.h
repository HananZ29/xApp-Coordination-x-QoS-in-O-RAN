/*
 * CMPS 342 project (H. Zoueihed), Nov 2026. Stage S1/S2 instrumentation.
 *
 * Passive measurement layer for the xapp-coord scenario. Hooks attach to existing ns-3 trace
 * sources, plus (S2) a pure-forwarding proxy on the eNB RLC->MAC SAP that records the buffer
 * status reports the RLC sends to the scheduler. Nothing here schedules simulator events or
 * draws random numbers; non-intrusiveness is verified by the S0 legacy regression.
 *
 * WINDOWS (S2): an event at simulation time t belongs to window j = floor((t - t0) / W),
 * computed in integer nanoseconds, covering [t0 + jW, t0 + (j+1)W). A row is written when the
 * window closes, with time_s = t0 + (j+1)W. An event exactly on a boundary therefore belongs to
 * the NEXT window regardless of event ordering. Events before t0 are excluded from window files.
 * t0 = traffic start (the legacy throughput sampler's origin).
 *
 * Outputs (CSV unless noted):
 *   <out>-flows.csv      per UE per window: tx/rx packets, UNIQUE bytes (by UDP sequence number),
 *                        duplicates, goodput, one-way delay mean/max
 *   <out>-flowsum.csv    per UE: conservation, loss before the accounting cutoff, delay distribution
 *   <out>-radio.csv      per UE per window: serving RSRP, SINR (linear mean, mean of dB, min, max)
 *   <out>-meas.csv       raw UE RSRP/RSRQ measurements (serving and neighbour cells)
 *   <out>-prb.csv        per cell per window: DL data RBs radiated (from the PSD of every DL data
 *                        frame), utilisation over 25 physical RBs and over the 24 PF-schedulable RBs
 *   <out>-backlog.csv    per DRB per window: RLC transmission-queue bytes (time mean, max, end),
 *                        head-of-line delay (max, end), buffer-status reports, RLC drops
 *   <out>-phypower.csv   radiated TxPower changes per cell
 *   <out>-ctrl.csv       control-plane event log (power commands, handovers)
 *   <out>-ulsinr.csv     per (cell, rnti) per window: uplink SRS SINR (mean of dB, min), samples
 *   <out>-telemetry.csv  (optional) every E2 report received by the RIC: generation and arrival time
 *   <out>-prbsf.csv, <out>-dlsched.csv  (optional) per-frame PRB / MAC DCI verification traces
 */

#ifndef XAPP_METRICS_H
#define XAPP_METRICS_H

#include "ns3/applications-module.h"
#include "ns3/core-module.h"
#include "ns3/lte-module.h"
#include "ns3/network-module.h"
#include "ns3/spectrum-channel.h"

#include <string>
#include <vector>

namespace xapp
{

struct MetricsConfig
{
    std::string out;            //!< output prefix
    uint32_t nUe{0};            //!< number of UEs
    ns3::Time window;           //!< measurement window W
    ns3::Time windowOrigin;     //!< t0 (traffic start)
    ns3::Time statsStart;       //!< start of post-warm-up statistics
    ns3::Time simTime;          //!< end of simulation
    ns3::Time acctGuard;        //!< loss accounting: packets sent after simTime-guard excluded
    uint16_t nRbConfig{0};      //!< configured DL bandwidth in RBs (physical)
    uint16_t nRbSchedulable{0}; //!< RBs the PF Type-0 scheduler can allocate
    bool rawPrb{false};         //!< write per-frame PRB + MAC scheduling traces
    bool telemetry{false};      //!< write <out>-telemetry.csv
    std::vector<uint64_t> imsi; //!< IMSI per UE index
};

/** Cumulative per-cell counters since t = 0, for modelled E2 reporting (no lookahead). */
struct CellCumulative
{
    uint64_t dataRbSum{0};       //!< DL data RBs radiated
    uint64_t dataFrames{0};      //!< DL data frames
    double backlogByteSeconds{0}; //!< integral of the summed DRB RLC tx-queue bytes over time
    uint64_t backlogBytesNow{0}; //!< summed DRB RLC tx-queue bytes at this instant
    uint32_t maxHolMsNow{0};     //!< max head-of-line delay over DRBs at this instant
    uint64_t rlcDropBytes{0};    //!< RLC transmit-buffer drops
};

/** S4: cumulative per-UE DRB counters at one eNB (open bearers only), for per-UE service telemetry. */
struct UeCumulative
{
    uint16_t rnti{0};
    uint64_t imsi{0};
    uint64_t inBytes{0};   //!< PDCP PDU bytes offered to RLC (DL arrivals at the eNB)
    uint64_t outBytes{0};  //!< RLC payload bytes handed to MAC (DL served volume, first transmission)
    uint64_t dropBytes{0}; //!< RLC transmit-buffer drops
    uint64_t ackedBytes{0}; //!< RLC payload in DL TBs positively HARQ-acknowledged (delivered over the air)
    uint64_t harqLostBytes{0}; //!< RLC payload in DL TBs abandoned without ACK
    uint64_t backlogBytesNow{0};
    uint32_t maxHolMsNow{0};
    int64_t createdNs{0}; //!< creation time of the UE's oldest open DRB at this eNB
};

void MetricsOpen(const MetricsConfig& cfg);
void MetricsAttachFlow(uint32_t ue, ns3::Ptr<ns3::UdpClient> client, ns3::Ptr<ns3::UdpServer> server);
void MetricsAttachUeRadio(uint32_t ue, ns3::Ptr<ns3::LteUeNetDevice> dev);
void MetricsAttachDlChannel(ns3::Ptr<ns3::SpectrumChannel> dlChannel);
void MetricsAttachEnbMac(uint16_t cellId, ns3::Ptr<ns3::LteEnbNetDevice> dev);
/** S2: RLC buffer-status proxy on every DRB of this eNB (installed as DRBs are created). */
void MetricsAttachEnbRlc(ns3::Ptr<ns3::LteEnbNetDevice> dev);
/** S2: uplink SINR (SRS-based, eNB PHY ReportUeSinr) per (cell, rnti) per window -> <out>-ulsinr.csv */
void MetricsAttachEnbUlSinr(ns3::Ptr<ns3::LteEnbNetDevice> dev);
void MetricsAttachHandoverTraces();
/** S3: called whenever a cell's radiated power is observed to change (cell, old dBm, new dBm). */
void MetricsSetPhyPowerHook(void (*hook)(uint16_t, double, double));

void MetricsCtrlEvent(const std::string& event,
                      int cell,
                      int64_t imsi,
                      int targetCell,
                      int64_t reqId,
                      double tCreatedS,
                      double requestedDbm,
                      double oldDbm,
                      double newDbm,
                      const std::string& status);

void MetricsTpcCommand(uint16_t cellId,
                       uint64_t requestId,
                       ns3::Time createdAt,
                       double requestedDbm,
                       double oldDbm,
                       double appliedDbm,
                       const std::string& status);

/** Snapshot of cumulative counters for @p cellId at the current simulation time. */
CellCumulative MetricsCellCumulative(uint16_t cellId);
std::vector<UeCumulative> MetricsUeCumulative(uint16_t cellId);

/** Telemetry log row for a report received by the RIC (called by the telemetry sink). */
void MetricsTelemetryRow(const std::string& type,
                         uint64_t reporterE2NodeId,
                         ns3::Time reportTime,
                         const std::string& payload);

/** Close the window that ends now (called at every window boundary). */
void MetricsSampleWindow(const std::vector<uint16_t>& servingCell);

void MetricsFinish(const std::vector<std::string>& group, const std::vector<double>& offeredMbps);

// ---- test hooks (used only by the metrics unit test executable) ----
void MetricsTestTx(uint32_t ue);
void MetricsTestRx(uint32_t ue, ns3::Ptr<const ns3::Packet> p);
void MetricsTestBufferStatus(uint16_t cellId, uint16_t rnti, uint8_t lcid, uint32_t txQueueBytes,
                             uint16_t holMs);

} // namespace xapp

#endif // XAPP_METRICS_H
