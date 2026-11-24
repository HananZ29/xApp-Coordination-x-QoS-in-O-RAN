/*
 * CMPS 342 project (H. Zoueihed), Nov 2026. Stage S2 unit test for simulation/metrics.
 *
 * Drives the metrics trace sinks with a scripted event sequence (no LTE network) and writes the
 * normal metrics CSVs to --out. Expected values are asserted by
 * scripts/validation/s2_acceptance.py (check K.*). Scenario: W = 100 ms, t0 = 1.0 s,
 * stats start 1.0 s, end 2.0 s, accounting guard 0.1 s (cutoff 1.9 s), packets of 1024 bytes.
 *
 *  UE 0 packets (seq k = k-th transmission):
 *   seq0 tx 0.95 rx 0.97      before t0: excluded from windows, counted in flowsum
 *   seq1 tx 1.05 rx 1.10      rx EXACTLY on a boundary, scheduled BEFORE the window close
 *   seq2 tx 1.15 rx 1.20      rx EXACTLY on a boundary, scheduled AFTER the window close
 *   seq3 tx 1.22 rx 1.25, duplicate rx 1.27
 *   seq4 tx 1.30 never received  -> lost (sent before cutoff)
 *   seq5 tx 1.95 never received  -> not counted as lost (sent after cutoff)
 *  Backlog (cell 1, rnti 1, lcid 3), buffer status reports:
 *   1.40: 1000 B, HOL 5 ms;  1.45: 0 B, HOL 0
 *   1.60: 2000 B, HOL 7 ms  EXACTLY on a boundary, BEFORE the window close
 *   1.70: 0 B,   HOL 0      EXACTLY on a boundary, AFTER the window close
 */

#include "xapp-metrics.h"

#include "ns3/core-module.h"
#include "ns3/network-module.h"
#include "ns3/seq-ts-header.h"

#include <map>

using namespace ns3;

namespace
{
std::map<uint32_t, Ptr<Packet>> g_pkts;

void
Tx(uint32_t seq)
{
    SeqTsHeader h; // timestamp = now (transmit time)
    h.SetSeq(seq);
    Ptr<Packet> p = Create<Packet>(1024 - h.GetSerializedSize());
    p->AddHeader(h);
    g_pkts[seq] = p;
    xapp::MetricsTestTx(0);
}

void
Rx(uint32_t seq)
{
    xapp::MetricsTestRx(0, g_pkts.at(seq));
}

void
Bsr(uint32_t bytes, uint16_t hol)
{
    xapp::MetricsTestBufferStatus(1, 1, 3, bytes, hol);
}

void
Sample()
{
    xapp::MetricsSampleWindow({1});
}
} // namespace

int
main(int argc, char* argv[])
{
    std::string out = "metrics-test";
    CommandLine cmd(__FILE__);
    cmd.AddValue("out", "output prefix", out);
    cmd.Parse(argc, argv);

    xapp::MetricsConfig mc;
    mc.out = out;
    mc.nUe = 1;
    mc.window = MilliSeconds(100);
    mc.windowOrigin = Seconds(1.0);
    mc.statsStart = Seconds(1.0);
    mc.simTime = Seconds(2.0);
    mc.acctGuard = Seconds(0.1);
    mc.nRbConfig = 25;
    mc.nRbSchedulable = 24;
    mc.imsi = {1};
    xapp::MetricsOpen(mc);

    // Same-time events run in insertion order; insertion order below is deliberate.
    Simulator::Schedule(Seconds(0.95), &Tx, 0);
    Simulator::Schedule(Seconds(0.97), &Rx, 0);
    Simulator::Schedule(Seconds(1.05), &Tx, 1);
    Simulator::Schedule(Seconds(1.10), &Rx, 1); // before the close at 1.10
    Simulator::Schedule(Seconds(1.10), &Sample);
    Simulator::Schedule(Seconds(1.15), &Tx, 2);
    Simulator::Schedule(Seconds(1.20), &Sample);
    Simulator::Schedule(Seconds(1.20), &Rx, 2); // after the close at 1.20
    Simulator::Schedule(Seconds(1.22), &Tx, 3);
    Simulator::Schedule(Seconds(1.25), &Rx, 3);
    Simulator::Schedule(Seconds(1.27), &Rx, 3); // duplicate
    Simulator::Schedule(Seconds(1.30), &Tx, 4); // never received
    Simulator::Schedule(Seconds(1.40), &Bsr, 1000, 5);
    Simulator::Schedule(Seconds(1.45), &Bsr, 0, 0);
    Simulator::Schedule(Seconds(1.60), &Bsr, 2000, 7); // before the close at 1.60
    for (int k = 3; k <= 10; k++)
    {
        Time t = Seconds(1.0) + MilliSeconds(100 * k);
        Simulator::Schedule(t, &Sample);
        if (k == 7)
        {
            Simulator::Schedule(t, &Bsr, 0, 0); // after the close at 1.70
        }
    }
    Simulator::Schedule(Seconds(1.95), &Tx, 5); // after the accounting cutoff, never received

    Simulator::Run();
    xapp::MetricsFinish({"test"}, {1.0});
    Simulator::Destroy();
    return 0;
}
