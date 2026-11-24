/*
 * CMPS 342 project (H. Zoueihed), Nov 2026. Stage S1/S2 instrumentation; see xapp-metrics.h.
 */

#include "xapp-metrics.h"

#include "ns3/lte-amc.h"
#include "ns3/lte-control-messages.h"
#include "ns3/lte-enb-mac.h"
#include "ns3/lte-enb-phy-sap.h"
#include "ns3/lte-enb-phy.h"
#include "ns3/lte-enb-component-carrier-manager.h"
#include "ns3/lte-mac-sap.h"
#include "ns3/lte-radio-bearer-info.h"
#include "ns3/lte-pdcp.h"
#include "ns3/lte-rlc-header.h"
#include "ns3/lte-rlc.h"
#include "ns3/lte-spectrum-signal-parameters.h"
#include "ns3/object-map.h"
#include "ns3/seq-ts-header.h"
#include "ns3/spectrum-value.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <memory>
#include <numeric>
#include <sstream>
#include <tuple>

namespace xapp
{
using namespace ns3;

namespace
{
MetricsConfig g_cfg;
void (*g_phyPowerHook)(uint16_t, double, double) = nullptr; // S3 command lifecycle
bool g_open = false;
int64_t g_t0ns = 0; //!< window origin (ns)
int64_t g_wns = 1;  //!< window length (ns)

std::ofstream g_flows, g_flowsum, g_radio, g_meas, g_prb, g_backlog, g_phypower, g_ctrl,
    g_telemetry, g_prbsf, g_dlsched, g_ulsinr;

struct UlWin
{
    double dbSum{0};
    double dbMin{std::numeric_limits<double>::infinity()};
    uint64_t n{0};
};
std::map<std::pair<uint16_t, uint16_t>, std::map<int64_t, UlWin>> g_ul; // (cell, rnti) -> windows

std::string
Fmt(double v, int prec = 6)
{
    if (std::isnan(v))
    {
        return "";
    }
    std::ostringstream ss;
    ss << std::fixed << std::setprecision(prec) << v;
    return ss.str();
}

double
NowS()
{
    return Simulator::Now().GetSeconds();
}

int64_t
NowNs()
{
    return Simulator::Now().GetNanoSeconds();
}

/** Window index of time t (ns); -1 for t < t0. */
int64_t
WinIdx(int64_t tns)
{
    int64_t d = tns - g_t0ns;
    return d >= 0 ? d / g_wns : -1;
}

// ---------------- flows ----------------
struct FlowWin
{
    uint64_t tx{0}, rx{0}, unique{0}, dup{0}, uniqueBytes{0};
    double delaySumMs{0}, delayMaxMs{0};
};

struct Flow
{
    Ptr<UdpClient> client;
    std::vector<double> txTimeS;      //!< tx time of sequence number k (k-th successful send)
    std::vector<bool> received;       //!< unique reception flag per sequence number
    std::vector<double> delayMsStats; //!< delay of unique packets received after statsStart
    uint64_t rxPkts{0}, rxUnique{0}, dup{0}, seqOutOfRange{0};
    uint64_t statsUniqueBytes{0};
    std::map<int64_t, FlowWin> win;
};
std::vector<Flow> g_flow;

// ---------------- radio ----------------
struct RadioWin
{
    double rsrpW{0}, sinr{0}, sinrDbSum{0};
    double sinrDbMin{std::numeric_limits<double>::infinity()};
    double sinrDbMax{-std::numeric_limits<double>::infinity()};
    uint64_t n{0};
    uint16_t lastCell{0};
};
std::vector<std::map<int64_t, RadioWin>> g_radioWin;

// ---------------- PRB / power per cell ----------------
struct CellWin
{
    uint64_t dataFrames{0}, ctrlFrames{0}, rbSum{0};
    uint16_t rbMax{0};
};

struct Cell
{
    std::map<int64_t, CellWin> win;
    bool havePower{false};
    double lastPowerDbm{0};
    uint64_t cumRbSum{0}, cumDataFrames{0}, cumDropBytes{0};
};
std::map<uint16_t, Cell> g_cell;
Ptr<LteAmc> g_amc;

// ---------------- RLC backlog (S2) ----------------
struct BearerWin
{
    double byteNs{0}; //!< integral of queue bytes over time within the window
    uint64_t maxBytes{0};
    uint32_t maxHolMs{0};
    uint64_t reports{0};
    uint64_t dropBytes{0};
    uint64_t bsrInconsistent{0}; //!< reports where (reported - exact) is not an even number >= 0
    // Value carried in from the previous window. It counts toward this window's maxima only if
    // it was held for a positive time, i.e. no report arrived exactly at the window start.
    bool hasCarry{false};
    bool startSuperseded{false};
    uint64_t carryBytes{0};
    uint32_t carryHolMs{0};
};

struct Bearer
{
    uint16_t cell{0}, rnti{0};
    uint8_t lcid{0};
    uint64_t imsi{0};
    bool open{true};
    uint64_t bytes{0};   //!< current RLC tx-queue bytes, EXACT reconstruction (see Exact* below)
    uint64_t reported{0}; //!< last buffer status report value (RLC estimate incl. 2 B/SDU headers)
    uint32_t holMs{0};   //!< head-of-line delay of the last report (0 when the queue is empty)
    int64_t lastNs{0};   //!< time of last integration
    int64_t createdNs{0}; //!< creation time: no rows for windows that end at or before it
    double cumByteNs{0}; //!< integral since creation
    uint64_t cumIn{0};   //!< S4 telemetry: PDCP PDU bytes offered to this RLC since creation
    uint64_t cumOut{0};  //!< S4 telemetry: RLC payload bytes handed to MAC since creation
    uint64_t cumDrop{0}; //!< S4 telemetry: RLC transmit-buffer drop bytes since creation
    uint64_t cumAcked{0}; //!< S4 telemetry: RLC payload in DL TBs positively HARQ-acknowledged
    uint64_t cumHarqLost{0}; //!< S4 telemetry: RLC payload in DL TBs whose HARQ process was reused without ACK
    std::map<int64_t, BearerWin> win;
};
using BearerKey = std::tuple<uint16_t, uint16_t, uint8_t>;
std::map<BearerKey, Bearer> g_bearer;

/** Integrate the current value of bearer b from its last update up to tns. The interval never
 *  spans a window boundary because every boundary triggers an integration (window close). */
void
Integrate(Bearer& b, int64_t tns)
{
    if (tns > b.lastNs)
    {
        double area = double(b.bytes) * double(tns - b.lastNs);
        b.cumByteNs += area;
        int64_t idx = WinIdx(b.lastNs);
        if (idx >= 0)
        {
            b.win[idx].byteNs += area;
            // queue maxima count only values HELD for a positive duration: zero-duration transients
            // (e.g. PDCP-in then RLC-drop at the same instant) cannot inflate the maximum
            b.win[idx].maxBytes = std::max<uint64_t>(b.win[idx].maxBytes, b.bytes);
        }
        b.lastNs = tns;
    }
}

/** Set the exact queue value of bearer b at the current time (integrates the old value first). */
void
SetExact(Bearer& b, uint64_t bytes)
{
    int64_t now = NowNs();
    Integrate(b, now);
    b.bytes = bytes;
    if (bytes == 0)
    {
        b.holMs = 0;
    }
}

Bearer*
FindBearer(uint16_t cell, uint16_t rnti, uint8_t lcid)
{
    auto it = g_bearer.find({cell, rnti, lcid});
    return it == g_bearer.end() ? nullptr : &it->second;
}

/* EXACT RLC UM queue reconstruction (S2 fix). ns-3 RLC UM reports its buffer status on SDU
 * arrival and every 10 ms while non-empty, but NOT when the queue drains (lte-rlc-um.cc), so the
 * last report goes stale. The true queue m_txBufferSize is reconstructed from three observable
 * byte streams: + PDCP PDU into RLC (eNB PDCP TxPDU trace, fired before RLC accepts it),
 * - RLC transmit-buffer drop (TxDrop), - RLC PDU payload handed to MAC (TransmitPdu via the proxy,
 * PDU size minus the parsed RLC header). Each report is a consistency check:
 * reported = m_txBufferSize + 2 * (#SDUs queued) => (reported - exact) is even and >= 0. */
void
OnPdcpTx(uint16_t cell, uint16_t rnti, uint8_t lcid, uint16_t, uint8_t, uint32_t size)
{
    if (Bearer* b = FindBearer(cell, rnti, lcid))
    {
        b->cumIn += size;
        SetExact(*b, b->bytes + size);
    }
}

void
RlcPayloadOut(uint16_t cell, uint16_t rnti, uint8_t lcid, uint32_t payload)
{
    if (Bearer* b = FindBearer(cell, rnti, lcid))
    {
        b->cumOut += payload;
        SetExact(*b, b->bytes >= payload ? b->bytes - payload : 0);
    }
}

void
BufferStatus(uint16_t cell, uint16_t rnti, uint8_t lcid, uint32_t reported, uint16_t holMs)
{
    Bearer* b = FindBearer(cell, rnti, lcid);
    if (!b)
    {
        return; // not a tracked DRB
    }
    int64_t now = NowNs();
    Integrate(*b, now);
    b->reported = reported;
    b->holMs = (b->bytes == 0) ? 0 : holMs;
    int64_t idx = WinIdx(now);
    if (idx >= 0)
    {
        BearerWin& w = b->win[idx];
        w.maxHolMs = std::max<uint32_t>(w.maxHolMs, b->holMs);
        w.reports++;
        int64_t d = int64_t(reported) - int64_t(b->bytes);
        if (d < 0 || d % 2 != 0)
        {
            w.bsrInconsistent++;
        }
        if ((now - g_t0ns) % g_wns == 0)
        {
            w.startSuperseded = true;
        }
    }
}

/** Pure-forwarding proxy for the eNB RLC -> MAC SAP: records each buffer status report. */
/* S4 telemetry: eNB-side HARQ-acknowledged DL volume. The RLC payload of each DL transport block is
 * recorded per (cell, RNTI, HARQ process) when RLC hands it to MAC (TransmitPdu carries the HARQ
 * process id). The UE's DL HARQ feedback (PUCCH, ideal control message) reaches the eNB MAC through
 * the PHY SAP; an ACK credits the recorded payload as delivered. A new TB on a process that still
 * holds unacknowledged payload means the previous TB was abandoned after its last retransmission
 * (credited as HARQ-lost). eNB-observable information only. */
struct HarqTb
{
    int64_t ns{-1}; //!< time the TB was formed
    std::map<uint8_t, uint64_t> payload; //!< lcid -> RLC payload bytes
};
std::map<std::tuple<uint16_t, uint16_t, uint8_t>, HarqTb> g_harq;

Bearer* FindBearer(uint16_t cell, uint16_t rnti, uint8_t lcid);

void
HarqRecord(uint16_t cell, uint16_t rnti, uint8_t harq, uint8_t lcid, uint64_t payload)
{
    HarqTb& tb = g_harq[{cell, rnti, harq}];
    int64_t now = Simulator::Now().GetNanoSeconds();
    if (tb.ns != now)
    {
        for (auto& [l, bytes] : tb.payload) // previous TB on this process never ACKed
        {
            if (Bearer* b = FindBearer(cell, rnti, l))
            {
                b->cumHarqLost += bytes;
            }
        }
        tb.payload.clear();
        tb.ns = now;
    }
    tb.payload[lcid] += payload;
}

void
HarqFeedback(uint16_t cell, const DlInfoListElement_s& fb)
{
    bool ack = false;
    for (auto s : fb.m_harqStatus)
    {
        ack = ack || s == DlInfoListElement_s::ACK;
    }
    if (!ack)
    {
        return; // NACK: the MAC retransmits on the same process
    }
    auto it = g_harq.find({cell, fb.m_rnti, fb.m_harqProcessId});
    if (it == g_harq.end())
    {
        return;
    }
    for (auto& [l, bytes] : it->second.payload)
    {
        if (Bearer* b = FindBearer(cell, fb.m_rnti, l))
        {
            b->cumAcked += bytes;
        }
    }
    g_harq.erase(it);
}

class HarqProxy : public LteEnbPhySapUser
{
  public:
    HarqProxy(LteEnbPhySapUser* real, uint16_t cell)
        : m_real(real),
          m_cell(cell)
    {
    }

    void ReceivePhyPdu(Ptr<Packet> p) override
    {
        m_real->ReceivePhyPdu(p);
    }

    void SubframeIndication(uint32_t frameNo, uint32_t subframeNo) override
    {
        m_real->SubframeIndication(frameNo, subframeNo);
    }

    void ReceiveLteControlMessage(Ptr<LteControlMessage> msg) override
    {
        if (msg->GetMessageType() == LteControlMessage::DL_HARQ)
        {
            HarqFeedback(m_cell,
                         DynamicCast<DlHarqFeedbackLteControlMessage>(msg)->GetDlHarqFeedback());
        }
        m_real->ReceiveLteControlMessage(msg);
    }

    void ReceiveRachPreamble(uint32_t prachId) override
    {
        m_real->ReceiveRachPreamble(prachId);
    }

    void UlCqiReport(FfMacSchedSapProvider::SchedUlCqiInfoReqParameters ulcqi) override
    {
        m_real->UlCqiReport(ulcqi);
    }

    void UlInfoListElementHarqFeedback(UlInfoListElement_s params) override
    {
        m_real->UlInfoListElementHarqFeedback(params);
    }

    void DlInfoListElementHarqFeedback(DlInfoListElement_s params) override
    {
        m_real->DlInfoListElementHarqFeedback(params);
    }

  private:
    LteEnbPhySapUser* m_real;
    uint16_t m_cell;
};
std::vector<std::unique_ptr<HarqProxy>> g_harqProxies;

class BsrProxy : public LteMacSapProvider
{
  public:
    BsrProxy(LteMacSapProvider* real, uint16_t cell)
        : m_real(real),
          m_cell(cell)
    {
    }

    void TransmitPdu(TransmitPduParameters params) override
    {
        LteRlcHeader h;
        params.pdu->PeekHeader(h);
        uint32_t hdr = h.GetSerializedSize();
        uint32_t size = params.pdu->GetSize();
        RlcPayloadOut(m_cell, params.rnti, params.lcid, size > hdr ? size - hdr : 0);
        if (FindBearer(m_cell, params.rnti, params.lcid))
        {
            HarqRecord(m_cell, params.rnti, params.harqProcessId, params.lcid,
                       size > hdr ? size - hdr : 0);
        }
        m_real->TransmitPdu(params);
    }

    void ReportBufferStatus(ReportBufferStatusParameters params) override
    {
        BufferStatus(m_cell, params.rnti, params.lcid, params.txQueueSize, params.txQueueHolDelay);
        m_real->ReportBufferStatus(params);
    }

  private:
    LteMacSapProvider* m_real;
    uint16_t m_cell;
};
std::vector<std::unique_ptr<BsrProxy>> g_proxies; // live until program exit

// ---------------- trace sinks ----------------
void
OnClientTx(uint32_t ue, Ptr<const Packet>)
{
    Flow& f = g_flow[ue];
    f.txTimeS.push_back(NowS());
    int64_t idx = WinIdx(NowNs());
    if (idx >= 0)
    {
        f.win[idx].tx++;
    }
}

void
OnServerRx(uint32_t ue, Ptr<const Packet> p)
{
    Flow& f = g_flow[ue];
    int64_t idx = WinIdx(NowNs());
    FlowWin dummy;
    FlowWin& w = idx >= 0 ? f.win[idx] : dummy;
    f.rxPkts++;
    w.rx++;
    SeqTsHeader h;
    p->PeekHeader(h);
    uint32_t seq = h.GetSeq();
    double delayMs = (Simulator::Now() - h.GetTs()).GetSeconds() * 1e3;
    if (seq >= f.received.size())
    {
        f.received.resize(seq + 1, false);
    }
    if (f.received[seq])
    {
        f.dup++;
        w.dup++;
        return;
    }
    f.received[seq] = true;
    f.rxUnique++;
    w.unique++;
    w.uniqueBytes += p->GetSize();
    w.delaySumMs += delayMs;
    w.delayMaxMs = std::max(w.delayMaxMs, delayMs);
    if (Simulator::Now() >= g_cfg.statsStart)
    {
        f.statsUniqueBytes += p->GetSize();
        f.delayMsStats.push_back(delayMs);
    }
}

void
OnRsrpSinr(uint32_t ue, uint16_t cellId, uint16_t, double rsrpW, double sinr, uint8_t)
{
    int64_t idx = WinIdx(NowNs());
    if (idx < 0)
    {
        return;
    }
    RadioWin& r = g_radioWin[ue][idx];
    r.rsrpW += rsrpW;
    r.sinr += sinr;
    double sdb = 10 * std::log10(sinr);
    r.sinrDbSum += sdb;
    r.sinrDbMin = std::min(r.sinrDbMin, sdb);
    r.sinrDbMax = std::max(r.sinrDbMax, sdb);
    r.n++;
    r.lastCell = cellId;
}

void
OnUlSinr(uint16_t cellId, uint16_t rnti, double sinrLinear, uint8_t)
{
    int64_t idx = WinIdx(NowNs());
    if (idx < 0)
    {
        return;
    }
    UlWin& w = g_ul[{cellId, rnti}][idx];
    double db = 10 * std::log10(sinrLinear);
    w.dbSum += db;
    w.dbMin = std::min(w.dbMin, db);
    w.n++;
}

void
OnUeMeas(uint32_t ue, uint16_t rnti, uint16_t cellId, double rsrpDbm, double rsrqDb, bool serving, uint8_t)
{
    g_meas << Fmt(NowS(), 4) << "," << ue << "," << rnti << "," << cellId << "," << Fmt(rsrpDbm, 4)
           << "," << Fmt(rsrqDb, 4) << "," << (serving ? 1 : 0) << "\n";
}

void
OnDlTx(Ptr<SpectrumSignalParameters> params)
{
    uint16_t cellId = 0;
    bool data = false;
    if (auto d = DynamicCast<LteSpectrumSignalParametersDataFrame>(params))
    {
        cellId = d->cellId;
        data = true;
    }
    else if (auto c = DynamicCast<LteSpectrumSignalParametersDlCtrlFrame>(params))
    {
        cellId = c->cellId;
    }
    else
    {
        return;
    }

    // Active RBs = PSD bins with non-zero power (PSD per active RB = P / (N_RB * 180 kHz)).
    uint16_t nActive = 0;
    double maxPsd = 0;
    uint32_t mask = 0;
    uint16_t idx = 0;
    for (auto it = params->psd->ConstValuesBegin(); it != params->psd->ConstValuesEnd(); ++it, ++idx)
    {
        if (*it > 0)
        {
            nActive++;
            maxPsd = std::max(maxPsd, *it);
            if (idx < 32)
            {
                mask |= (1u << idx);
            }
        }
    }

    Cell& c = g_cell[cellId];
    int64_t w = WinIdx(NowNs());
    if (data)
    {
        c.cumDataFrames++;
        c.cumRbSum += nActive;
        if (w >= 0)
        {
            CellWin& cw = c.win[w];
            cw.dataFrames++;
            cw.rbSum += nActive;
            cw.rbMax = std::max(cw.rbMax, nActive);
        }
    }
    else if (w >= 0)
    {
        c.win[w].ctrlFrames++;
    }

    double powerDbm = std::numeric_limits<double>::quiet_NaN();
    if (nActive > 0)
    {
        powerDbm = 10.0 * std::log10(maxPsd * g_cfg.nRbConfig * 180000.0) + 30.0;
        if (!c.havePower || std::fabs(powerDbm - c.lastPowerDbm) > 1e-6)
        {
            g_phypower << Fmt(NowS(), 7) << "," << cellId << ","
                       << (c.havePower ? Fmt(c.lastPowerDbm, 4) : std::string()) << ","
                       << Fmt(powerDbm, 4) << "," << (data ? "data" : "ctrl") << "\n";
            if (c.havePower && g_phyPowerHook)
            {
                g_phyPowerHook(cellId, c.lastPowerDbm, powerDbm);
            }
            if (c.havePower)
            {
                MetricsCtrlEvent("phy_tx_power_observed", cellId, -1, -1, -1, NAN, NAN,
                                 c.lastPowerDbm, powerDbm, data ? "data_frame" : "ctrl_frame");
            }
            c.havePower = true;
            c.lastPowerDbm = powerDbm;
        }
    }

    if (g_cfg.rawPrb && data)
    {
        std::ostringstream m;
        m << std::hex << mask;
        g_prbsf << Fmt(NowS(), 7) << "," << cellId << "," << nActive << ",0x" << m.str() << ","
                << Fmt(powerDbm, 4) << "\n";
    }
}

void
OnDlScheduling(uint16_t cellId, DlSchedulingCallbackInfo info)
{
    // RB counts n for which the 36.213 TB size of (mcs, n) equals the scheduled size. Used ONLY
    // to verify the PSD-based RB measurement offline, never as the measurement itself.
    auto candidates = [](uint8_t mcs, uint16_t sizeBytes) {
        std::ostringstream s;
        bool first = true;
        if (sizeBytes == 0)
        {
            return std::string();
        }
        for (int n = 1; n <= g_cfg.nRbConfig; n++)
        {
            if (g_amc->GetDlTbSizeFromMcs(mcs, n) / 8 == sizeBytes)
            {
                s << (first ? "" : ";") << n;
                first = false;
            }
        }
        return s.str();
    };
    g_dlsched << Fmt(NowS(), 7) << "," << cellId << "," << info.frameNo << "," << info.subframeNo
              << "," << info.rnti << "," << (int)info.mcsTb1 << "," << info.sizeTb1 << ","
              << (int)info.mcsTb2 << "," << info.sizeTb2 << ","
              << candidates(info.mcsTb1, info.sizeTb1) << "\n";
}

void
OnRlcDrop(uint16_t cell, uint16_t rnti, uint8_t lcid, Ptr<const Packet> p)
{
    auto it = g_bearer.find({cell, rnti, lcid});
    g_cell[cell].cumDropBytes += p->GetSize();
    if (it != g_bearer.end())
    {
        int64_t idx = WinIdx(NowNs());
        if (idx >= 0)
        {
            it->second.win[idx].dropBytes += p->GetSize();
        }
        it->second.cumDrop += p->GetSize();
        // the PDCP PDU was counted in by OnPdcpTx but never entered the RLC buffer
        SetExact(it->second, it->second.bytes >= p->GetSize() ? it->second.bytes - p->GetSize() : 0);
    }
}

/** DRB created at an eNB: wrap its RLC's MAC SAP with a recording proxy. */
void
OnDrbCreated(Ptr<LteEnbNetDevice> dev, uint64_t imsi, uint16_t cellId, uint16_t rnti, uint8_t lcid)
{
    Ptr<UeManager> ue = dev->GetRrc()->GetUeManager(rnti);
    ObjectMapValue drbs;
    ue->GetAttribute("DataRadioBearerMap", drbs);
    Ptr<LteRlc> rlc;
    Ptr<LtePdcp> pdcp;
    for (auto it = drbs.Begin(); it != drbs.End(); ++it)
    {
        Ptr<LteDataRadioBearerInfo> drb = it->second->GetObject<LteDataRadioBearerInfo>();
        UintegerValue lc;
        drb->GetAttribute("logicalChannelIdentity", lc); // lower-case in ns-3.42 (lte-radio-bearer-info.cc)
        if (lc.Get() == lcid)
        {
            PointerValue pv;
            drb->GetAttribute("LteRlc", pv);
            rlc = pv.Get<LteRlc>();
            PointerValue pp;
            drb->GetAttribute("LtePdcp", pp);
            pdcp = pp.Get<LtePdcp>();
        }
    }
    NS_ABORT_MSG_IF(!rlc, "DRB lcid " << +lcid << " of rnti " << rnti << " not found");
    // Same provider LteHelper gave the RRC (lte-helper.cc:657), so forwarding is exact.
    LteMacSapProvider* real = dev->GetComponentCarrierManager()->GetLteMacSapProvider();
    g_proxies.push_back(std::make_unique<BsrProxy>(real, cellId));
    rlc->SetLteMacSapProvider(g_proxies.back().get());
    rlc->TraceConnectWithoutContext("TxDrop", MakeBoundCallback(&OnRlcDrop, cellId, rnti, lcid));
    NS_ABORT_MSG_IF(!pdcp, "DRB PDCP not found");
    pdcp->TraceConnectWithoutContext("TxPDU", MakeBoundCallback(&OnPdcpTx, cellId, rnti, lcid));

    Bearer b;
    b.cell = cellId;
    b.rnti = rnti;
    b.lcid = lcid;
    b.imsi = imsi;
    b.lastNs = NowNs();
    b.createdNs = b.lastNs;
    g_bearer[{cellId, rnti, lcid}] = b;
}

void
OnEnbUeState(uint64_t imsi, uint16_t cellId, uint16_t rnti, UeManager::State from, UeManager::State to)
{
    // eNB-side UE state machine (S2): needed because UE-side and eNB-side states can disagree
    static const char* names[] = {"INITIAL_RANDOM_ACCESS", "CONNECTION_SETUP", "CONNECTION_REJECTED",
                                  "ATTACH_REQUEST", "CONNECTED_NORMALLY", "CONNECTION_RECONFIGURATION",
                                  "CONNECTION_REESTABLISHMENT", "HANDOVER_PREPARATION", "HANDOVER_JOINING",
                                  "HANDOVER_PATH_SWITCH", "HANDOVER_LEAVING"};
    auto nm = [](UeManager::State st) {
        int i = static_cast<int>(st);
        return (i >= 0 && i < 11) ? std::string(names[i]) : std::to_string(i);
    };
    MetricsCtrlEvent("enb_ue_state", cellId, imsi, -1, rnti, NAN, NAN, NAN, NAN, nm(from) + ">" + nm(to));
}

void
OnNewUeContext(Ptr<LteEnbNetDevice> dev, uint16_t, uint16_t rnti)
{
    Ptr<UeManager> ue = dev->GetRrc()->GetUeManager(rnti);
    ue->TraceConnectWithoutContext("DrbCreated", MakeBoundCallback(&OnDrbCreated, dev));
    ue->TraceConnectWithoutContext("StateTransition", MakeCallback(&OnEnbUeState));
}

void
OnConnectionRelease(uint64_t, uint16_t cellId, uint16_t rnti)
{
    for (auto& [key, b] : g_bearer)
    {
        if (b.cell == cellId && b.rnti == rnti && b.open)
        {
            Integrate(b, NowNs());
            b.bytes = 0;
            b.holMs = 0;
            b.open = false;
        }
    }
    MetricsCtrlEvent("ue_context_released_enb", cellId, -1, -1, -1, NAN, NAN, NAN, NAN, "");
}

void
OnEnbHoStart(uint64_t imsi, uint16_t cellId, uint16_t, uint16_t targetCellId)
{
    MetricsCtrlEvent("ho_start_enb", cellId, imsi, targetCellId, -1, NAN, NAN, NAN, NAN, "");
}

void
OnEnbHoEndOk(uint64_t imsi, uint16_t cellId, uint16_t)
{
    MetricsCtrlEvent("ho_end_ok_enb", cellId, imsi, -1, -1, NAN, NAN, NAN, NAN, "");
}

void
OnUeHoStart(uint64_t imsi, uint16_t cellId, uint16_t, uint16_t targetCellId)
{
    MetricsCtrlEvent("ho_start_ue", cellId, imsi, targetCellId, -1, NAN, NAN, NAN, NAN, "");
}

void
OnUeHoEndOk(uint64_t imsi, uint16_t cellId, uint16_t)
{
    MetricsCtrlEvent("ho_end_ok_ue", cellId, imsi, -1, -1, NAN, NAN, NAN, NAN, "");
}

void
OnUeHoEndError(uint64_t imsi, uint16_t cellId, uint16_t)
{
    MetricsCtrlEvent("ho_end_error_ue", cellId, imsi, -1, -1, NAN, NAN, NAN, NAN, "");
}

void
OnUeConnEstablished(uint64_t imsi, uint16_t cellId, uint16_t)
{
    MetricsCtrlEvent("rrc_connection_established_ue", cellId, imsi, -1, -1, NAN, NAN, NAN, NAN, "");
}

void
OnUeConnTimeout(uint64_t imsi, uint16_t cellId, uint16_t, uint8_t count)
{
    MetricsCtrlEvent("rrc_connection_timeout_ue", cellId, imsi, -1, count, NAN, NAN, NAN, NAN, "");
}

void
OnUeCellSelError(uint64_t imsi, uint16_t cellId)
{
    MetricsCtrlEvent("initial_cell_selection_error_ue", cellId, imsi, -1, -1, NAN, NAN, NAN, NAN, "");
}

void
OnUeRaError(uint64_t imsi, uint16_t cellId, uint16_t)
{
    MetricsCtrlEvent("random_access_error_ue", cellId, imsi, -1, -1, NAN, NAN, NAN, NAN, "");
}

void
OnEnbMeasReport(uint64_t imsi, uint16_t cellId, uint16_t, LteRrcSap::MeasurementReport rep)
{
    // RRC measurement report received over SRB1 (ANR / handover measurement configurations)
    MetricsCtrlEvent("rrc_meas_report_rx_enb", cellId, imsi, -1, rep.measResults.measId, NAN, NAN,
                     NAN, NAN, rep.measResults.haveMeasResultNeighCells ? "with_neighbours" : "serving_only");
}

void
OnUeRlf(uint64_t imsi, uint16_t cellId, uint16_t)
{
    MetricsCtrlEvent("radio_link_failure_ue", cellId, imsi, -1, -1, NAN, NAN, NAN, NAN, "");
}

double
Percentile(std::vector<double> v, double q)
{
    if (v.empty())
    {
        return NAN;
    }
    std::sort(v.begin(), v.end()); // nearest-rank definition
    size_t rank = static_cast<size_t>(std::ceil(q * v.size()));
    rank = std::max<size_t>(rank, 1);
    return v[rank - 1];
}
} // namespace

void
MetricsOpen(const MetricsConfig& cfg)
{
    g_cfg = cfg;
    g_t0ns = cfg.windowOrigin.GetNanoSeconds();
    g_wns = cfg.window.GetNanoSeconds();
    NS_ABORT_MSG_IF(g_wns <= 0, "window must be positive");
    NS_ABORT_MSG_IF(g_wns % 1000000 != 0, "window must be a whole number of 1 ms TTIs");
    g_flow.assign(cfg.nUe, Flow());
    g_radioWin.assign(cfg.nUe, {});
    g_flows.open(cfg.out + "-flows.csv");
    g_flows << "time_s,ue,imsi,tx_pkts,rx_pkts,rx_unique_pkts,dup_pkts,rx_unique_bytes,"
               "goodput_mbps,delay_mean_ms,delay_max_ms\n";
    g_flowsum.open(cfg.out + "-flowsum.csv");
    g_flowsum << "ue,imsi,group,offered_mbps,tx_pkts,client_tx_bytes,rx_pkts,rx_unique_pkts,"
                 "dup_pkts,seq_out_of_range,never_received_pkts,acct_cutoff_s,tx_before_cutoff,"
                 "rx_unique_before_cutoff,lost_before_cutoff,loss_ratio_before_cutoff,"
                 "stats_goodput_mbps,stats_delay_n,delay_mean_ms,delay_p50_ms,delay_p95_ms,"
                 "delay_p99_ms,delay_max_ms\n";
    g_radio.open(cfg.out + "-radio.csv");
    // sinr_db: 10log10 of the linear mean over TTIs; sinr_db_mean_of_db: mean of per-TTI dB.
    // ns-3 default UsePdschForCqiGeneration=true: per-TTI SINR = PDCCH RS power / (PDSCH
    // interference + noise), so TTIs without neighbour data have SINR = SNR (S1 report §4).
    g_radio << "time_s,ue,cell_id,samples,rsrp_dbm,sinr_db,sinr_db_mean_of_db,sinr_db_min,"
               "sinr_db_max\n";
    g_meas.open(cfg.out + "-meas.csv");
    g_meas << "time_s,ue,rnti,cell_id,rsrp_dbm,rsrq_db,serving\n";
    g_prb.open(cfg.out + "-prb.csv");
    g_prb << "time_s,cell,subframes,data_frames,ctrl_frames,data_rb_sum,data_rb_max,"
             "prb_util_cfg,prb_util_sched\n";
    g_backlog.open(cfg.out + "-backlog.csv");
    // mean/max/end_bytes: EXACT RLC queue (reconstructed); reported_end_bytes: last buffer status report
    g_backlog << "time_s,cell,rnti,imsi,lcid,open,reports,mean_bytes,max_bytes,end_bytes,"
                 "max_hol_ms,end_hol_ms,drop_bytes,reported_end_bytes,bsr_inconsistent\n";
    g_ulsinr.open(cfg.out + "-ulsinr.csv");
    g_ulsinr << "time_s,cell,rnti,samples,ul_sinr_db_mean_of_db,ul_sinr_db_min\n";
    g_phypower.open(cfg.out + "-phypower.csv");
    g_phypower << "time_s,cell,old_dbm,new_dbm,frame_type\n";
    g_ctrl.open(cfg.out + "-ctrl.csv");
    g_ctrl << "time_s,event,cell,imsi,target_cell,req_id,t_created_s,requested_dbm,old_dbm,"
              "new_dbm,status\n";
    if (cfg.telemetry)
    {
        g_telemetry.open(cfg.out + "-telemetry.csv");
        g_telemetry << "arrival_s,type,reporter_e2_id,report_time_s,age_ms,payload\n";
    }
    if (cfg.rawPrb)
    {
        g_prbsf.open(cfg.out + "-prbsf.csv");
        g_prbsf << "time_s,cell,n_rb,rb_mask,tx_power_dbm\n";
        g_dlsched.open(cfg.out + "-dlsched.csv");
        g_dlsched << "time_s,cell,frame,subframe,rnti,mcs1,size1_bytes,mcs2,size2_bytes,"
                     "nrb_candidates_tb1\n";
        g_amc = CreateObject<LteAmc>();
    }
    g_open = true;
}

void
MetricsAttachFlow(uint32_t ue, Ptr<UdpClient> client, Ptr<UdpServer> server)
{
    g_flow[ue].client = client;
    if (client)
    {
        client->TraceConnectWithoutContext("Tx", MakeBoundCallback(&OnClientTx, ue));
    }
    server->TraceConnectWithoutContext("Rx", MakeBoundCallback(&OnServerRx, ue));
}

void
MetricsAttachUeRadio(uint32_t ue, Ptr<LteUeNetDevice> dev)
{
    Ptr<LteUePhy> phy = dev->GetPhy();
    phy->TraceConnectWithoutContext("ReportCurrentCellRsrpSinr", MakeBoundCallback(&OnRsrpSinr, ue));
    phy->TraceConnectWithoutContext("ReportUeMeasurements", MakeBoundCallback(&OnUeMeas, ue));
}

void
MetricsAttachDlChannel(Ptr<SpectrumChannel> dlChannel)
{
    dlChannel->TraceConnectWithoutContext("TxSigParams", MakeCallback(&OnDlTx));
}

void
MetricsAttachEnbMac(uint16_t cellId, Ptr<LteEnbNetDevice> dev)
{
    // S4 telemetry: observe DL HARQ feedback on the PHY->MAC SAP (pure forwarding proxy)
    g_harqProxies.push_back(std::make_unique<HarqProxy>(dev->GetMac()->GetLteEnbPhySapUser(), cellId));
    dev->GetPhy()->SetLteEnbPhySapUser(g_harqProxies.back().get());
    if (g_cfg.rawPrb)
    {
        dev->GetMac()->TraceConnectWithoutContext("DlScheduling",
                                                  MakeBoundCallback(&OnDlScheduling, cellId));
    }
}

void
MetricsAttachEnbRlc(Ptr<LteEnbNetDevice> dev)
{
    dev->GetRrc()->TraceConnectWithoutContext("NewUeContext", MakeBoundCallback(&OnNewUeContext, dev));
    dev->GetRrc()->TraceConnectWithoutContext("NotifyConnectionRelease",
                                              MakeCallback(&OnConnectionRelease));
}

void
MetricsAttachEnbUlSinr(Ptr<LteEnbNetDevice> dev)
{
    dev->GetPhy()->TraceConnectWithoutContext("ReportUeSinr", MakeCallback(&OnUlSinr));
}

void
MetricsSetPhyPowerHook(void (*hook)(uint16_t, double, double))
{
    g_phyPowerHook = hook;
}

void
MetricsAttachHandoverTraces()
{
    Config::ConnectWithoutContext("/NodeList/*/DeviceList/*/LteEnbRrc/HandoverStart",
                                  MakeCallback(&OnEnbHoStart));
    Config::ConnectWithoutContext("/NodeList/*/DeviceList/*/LteEnbRrc/HandoverEndOk",
                                  MakeCallback(&OnEnbHoEndOk));
    Config::ConnectWithoutContext("/NodeList/*/DeviceList/*/LteUeRrc/HandoverStart",
                                  MakeCallback(&OnUeHoStart));
    Config::ConnectWithoutContext("/NodeList/*/DeviceList/*/LteUeRrc/HandoverEndOk",
                                  MakeCallback(&OnUeHoEndOk));
    Config::ConnectWithoutContext("/NodeList/*/DeviceList/*/LteUeRrc/HandoverEndError",
                                  MakeCallback(&OnUeHoEndError));
    Config::ConnectWithoutContext("/NodeList/*/DeviceList/*/LteUeRrc/RadioLinkFailure",
                                  MakeCallback(&OnUeRlf));
    Config::ConnectWithoutContext("/NodeList/*/DeviceList/*/LteEnbRrc/RecvMeasurementReport",
                                  MakeCallback(&OnEnbMeasReport));
    Config::ConnectWithoutContext("/NodeList/*/DeviceList/*/LteUeRrc/ConnectionEstablished",
                                  MakeCallback(&OnUeConnEstablished));
    Config::ConnectWithoutContext("/NodeList/*/DeviceList/*/LteUeRrc/ConnectionTimeout",
                                  MakeCallback(&OnUeConnTimeout));
    Config::ConnectWithoutContext("/NodeList/*/DeviceList/*/LteUeRrc/InitialCellSelectionEndError",
                                  MakeCallback(&OnUeCellSelError));
    Config::ConnectWithoutContext("/NodeList/*/DeviceList/*/LteUeRrc/RandomAccessError",
                                  MakeCallback(&OnUeRaError));
}

void
MetricsCtrlEvent(const std::string& event,
                 int cell,
                 int64_t imsi,
                 int targetCell,
                 int64_t reqId,
                 double tCreatedS,
                 double requestedDbm,
                 double oldDbm,
                 double newDbm,
                 const std::string& status)
{
    if (!g_open)
    {
        return;
    }
    auto i = [](int64_t v) { return v < 0 ? std::string() : std::to_string(v); };
    g_ctrl << Fmt(NowS(), 7) << "," << event << "," << i(cell) << "," << i(imsi) << ","
           << i(targetCell) << "," << i(reqId) << "," << Fmt(tCreatedS, 7) << ","
           << Fmt(requestedDbm, 4) << "," << Fmt(oldDbm, 4) << "," << Fmt(newDbm, 4) << ","
           << status << "\n";
}

void
MetricsTpcCommand(uint16_t cellId,
                  uint64_t requestId,
                  Time createdAt,
                  double requestedDbm,
                  double oldDbm,
                  double appliedDbm,
                  const std::string& status)
{
    MetricsCtrlEvent("tpc_cmd_rx_enb", cellId, -1, -1, static_cast<int64_t>(requestId),
                     createdAt.IsNegative() ? NAN : createdAt.GetSeconds(), requestedDbm, oldDbm,
                     appliedDbm, status);
}

CellCumulative
MetricsCellCumulative(uint16_t cellId)
{
    CellCumulative c;
    auto it = g_cell.find(cellId);
    if (it != g_cell.end())
    {
        c.dataRbSum = it->second.cumRbSum;
        c.dataFrames = it->second.cumDataFrames;
        c.rlcDropBytes = it->second.cumDropBytes;
    }
    int64_t now = NowNs();
    for (auto& [key, b] : g_bearer)
    {
        if (b.cell != cellId)
        {
            continue;
        }
        Integrate(b, now);
        c.backlogByteSeconds += b.cumByteNs * 1e-9;
        if (b.open)
        {
            c.backlogBytesNow += b.bytes;
            c.maxHolMsNow = std::max(c.maxHolMsNow, b.holMs);
        }
    }
    return c;
}

std::vector<UeCumulative>
MetricsUeCumulative(uint16_t cellId)
{
    std::map<uint16_t, UeCumulative> m;
    for (auto& [key, b] : g_bearer)
    {
        if (b.cell != cellId || !b.open)
        {
            continue;
        }
        UeCumulative& u = m[b.rnti];
        u.rnti = b.rnti;
        u.imsi = b.imsi;
        u.inBytes += b.cumIn;
        u.outBytes += b.cumOut;
        u.dropBytes += b.cumDrop;
        u.ackedBytes += b.cumAcked;
        u.harqLostBytes += b.cumHarqLost;
        u.backlogBytesNow += b.bytes;
        u.createdNs = u.createdNs == 0 ? b.createdNs : std::min(u.createdNs, b.createdNs);
        u.maxHolMsNow = std::max(u.maxHolMsNow, b.holMs);
    }
    std::vector<UeCumulative> v;
    for (auto& [r, u] : m)
    {
        v.push_back(u);
    }
    return v;
}

void
MetricsTelemetryRow(const std::string& type,
                    uint64_t reporterE2NodeId,
                    Time reportTime,
                    const std::string& payload)
{
    if (!g_open || !g_cfg.telemetry)
    {
        return;
    }
    g_telemetry << Fmt(NowS(), 7) << "," << type << "," << reporterE2NodeId << ","
                << Fmt(reportTime.GetSeconds(), 7) << ","
                << Fmt((Simulator::Now() - reportTime).GetSeconds() * 1e3, 4) << ",\"" << payload
                << "\"\n";
}

void
MetricsSampleWindow(const std::vector<uint16_t>& servingCell)
{
    int64_t now = NowNs();
    NS_ABORT_MSG_IF((now - g_t0ns) % g_wns != 0, "window close off the window grid");
    int64_t k = WinIdx(now) - 1; // window that ends now
    double nowS = NowS();
    double w = g_cfg.window.GetSeconds();
    for (uint32_t ue = 0; ue < g_cfg.nUe; ue++)
    {
        Flow& f = g_flow[ue];
        FlowWin fw;
        if (auto it = f.win.find(k); it != f.win.end())
        {
            fw = it->second;
        }
        g_flows << Fmt(nowS, 3) << "," << ue << "," << g_cfg.imsi[ue] << "," << fw.tx << ","
                << fw.rx << "," << fw.unique << "," << fw.dup << "," << fw.uniqueBytes << ","
                << Fmt(fw.uniqueBytes * 8.0 / w / 1e6, 6) << ","
                << (fw.unique ? Fmt(fw.delaySumMs / fw.unique, 4) : std::string()) << ","
                << (fw.unique ? Fmt(fw.delayMaxMs, 4) : std::string()) << "\n";
        f.win.erase(f.win.begin(), f.win.upper_bound(k));

        RadioWin r;
        if (auto it = g_radioWin[ue].find(k); it != g_radioWin[ue].end())
        {
            r = it->second;
        }
        // Linear averages over the window, then converted: RSRP W -> dBm, SINR -> dB.
        g_radio << Fmt(nowS, 3) << "," << ue << "," << (r.n ? r.lastCell : servingCell[ue]) << ","
                << r.n << "," << (r.n ? Fmt(10 * std::log10(r.rsrpW / r.n) + 30, 4) : std::string())
                << "," << (r.n ? Fmt(10 * std::log10(r.sinr / r.n), 4) : std::string()) << ","
                << (r.n ? Fmt(r.sinrDbSum / r.n, 4) : std::string()) << ","
                << (r.n ? Fmt(r.sinrDbMin, 4) : std::string()) << ","
                << (r.n ? Fmt(r.sinrDbMax, 4) : std::string()) << "\n";
        g_radioWin[ue].erase(g_radioWin[ue].begin(), g_radioWin[ue].upper_bound(k));
    }
    uint64_t subframes = static_cast<uint64_t>(g_wns / 1000000); // 1 ms TTI
    for (auto& [cellId, c] : g_cell)
    {
        CellWin cw;
        if (auto it = c.win.find(k); it != c.win.end())
        {
            cw = it->second;
        }
        g_prb << Fmt(nowS, 3) << "," << cellId << "," << subframes << "," << cw.dataFrames << ","
              << cw.ctrlFrames << "," << cw.rbSum << "," << cw.rbMax << ","
              << Fmt(double(cw.rbSum) / (double(g_cfg.nRbConfig) * subframes), 6) << ","
              << Fmt(double(cw.rbSum) / (double(g_cfg.nRbSchedulable) * subframes), 6) << "\n";
        c.win.erase(c.win.begin(), c.win.upper_bound(k));
    }
    for (auto& [key, wins] : g_ul)
    {
        if (auto it = wins.find(k); it != wins.end() && it->second.n > 0)
        {
            g_ulsinr << Fmt(nowS, 3) << "," << key.first << "," << key.second << "," << it->second.n
                     << "," << Fmt(it->second.dbSum / it->second.n, 4) << ","
                     << Fmt(it->second.dbMin, 4) << "\n";
        }
        wins.erase(wins.begin(), wins.upper_bound(k));
    }
    for (auto& [key, b] : g_bearer)
    {
        Integrate(b, now); // closes [last, now) inside window k
        BearerWin bw;
        bool hadWin = false;
        if (auto it = b.win.find(k); it != b.win.end())
        {
            bw = it->second;
            hadWin = true;
        }
        if ((b.open || hadWin) && now > b.createdNs)
        {
            bool carry = bw.hasCarry && !bw.startSuperseded;
            uint64_t maxB = bw.maxBytes; // held-value maxima (see Integrate)
            uint32_t maxH = std::max<uint32_t>(bw.maxHolMs, carry ? bw.carryHolMs : 0);
            g_backlog << Fmt(nowS, 3) << "," << b.cell << "," << b.rnti << "," << b.imsi << ","
                      << +b.lcid << "," << (b.open ? 1 : 0) << "," << bw.reports << ","
                      << Fmt(bw.byteNs / double(g_wns), 2) << "," << maxB << "," << b.bytes << ","
                      << maxH << "," << b.holMs << "," << bw.dropBytes << "," << b.reported << ","
                      << bw.bsrInconsistent << "\n";
        }
        b.win.erase(b.win.begin(), b.win.upper_bound(k));
        if (b.open)
        {
            // the value held at the start of the next window (see BearerWin::hasCarry)
            BearerWin& nw = b.win[k + 1];
            nw.hasCarry = true;
            nw.carryBytes = b.bytes;
            nw.carryHolMs = b.holMs;
        }
    }
}

void
MetricsFinish(const std::vector<std::string>& group, const std::vector<double>& offeredMbps)
{
    double cutoff = (g_cfg.simTime - g_cfg.acctGuard).GetSeconds();
    double statsDur = (g_cfg.simTime - g_cfg.statsStart).GetSeconds();
    for (uint32_t ue = 0; ue < g_cfg.nUe; ue++)
    {
        Flow& f = g_flow[ue];
        uint64_t txTotal = f.txTimeS.size();
        uint64_t txBefore = std::count_if(f.txTimeS.begin(), f.txTimeS.end(),
                                          [cutoff](double t) { return t < cutoff; });
        // Sequence numbers are assigned in send order, so seq < txBefore <=> sent before cutoff.
        uint64_t rxBefore = 0;
        for (uint64_t s = 0; s < std::min<uint64_t>(txBefore, f.received.size()); s++)
        {
            rxBefore += f.received[s] ? 1 : 0;
        }
        for (uint64_t s = txTotal; s < f.received.size(); s++)
        {
            f.seqOutOfRange += f.received[s] ? 1 : 0;
        }
        uint64_t clientBytes = f.client ? f.client->GetTotalTx() : 0;
        const auto& d = f.delayMsStats;
        double mean = d.empty() ? NAN : std::accumulate(d.begin(), d.end(), 0.0) / d.size();
        double mx = d.empty() ? NAN : *std::max_element(d.begin(), d.end());
        g_flowsum << ue << "," << g_cfg.imsi[ue] << "," << group[ue] << "," << offeredMbps[ue] << ","
                  << txTotal << "," << clientBytes << "," << f.rxPkts << "," << f.rxUnique << ","
                  << f.dup << "," << f.seqOutOfRange << "," << (txTotal - f.rxUnique) << ","
                  << Fmt(cutoff, 3) << "," << txBefore << "," << rxBefore << ","
                  << (txBefore - rxBefore) << ","
                  << (txBefore ? Fmt(double(txBefore - rxBefore) / txBefore, 6) : std::string())
                  << "," << Fmt(f.statsUniqueBytes * 8.0 / statsDur / 1e6, 6) << "," << d.size()
                  << "," << Fmt(mean, 4) << "," << Fmt(Percentile(d, 0.50), 4) << ","
                  << Fmt(Percentile(d, 0.95), 4) << "," << Fmt(Percentile(d, 0.99), 4) << ","
                  << Fmt(mx, 4) << "\n";
    }
    for (auto* s : {&g_flows, &g_flowsum, &g_radio, &g_meas, &g_prb, &g_backlog, &g_phypower,
                    &g_ctrl, &g_telemetry, &g_prbsf, &g_dlsched, &g_ulsinr})
    {
        if (s->is_open())
        {
            s->close();
        }
    }
    g_open = false;
}

void
MetricsTestTx(uint32_t ue)
{
    OnClientTx(ue, nullptr);
}

void
MetricsTestRx(uint32_t ue, Ptr<const Packet> p)
{
    OnServerRx(ue, p);
}

void
MetricsTestBufferStatus(uint16_t cellId, uint16_t rnti, uint8_t lcid, uint32_t txQueueBytes,
                        uint16_t holMs)
{
    BearerKey key{cellId, rnti, lcid};
    if (g_bearer.find(key) == g_bearer.end())
    {
        Bearer b;
        b.cell = cellId;
        b.rnti = rnti;
        b.lcid = lcid;
        b.lastNs = NowNs();
        b.createdNs = b.lastNs;
        g_bearer[key] = b;
    }
    // test hook: set the exact queue directly, then deliver a consistent report (reported = exact)
    SetExact(g_bearer[key], txQueueBytes);
    BufferStatus(cellId, rnti, lcid, txQueueBytes, holMs);
}

} // namespace xapp
