/**
 * xapp-coord.cc -- CMPS 342 project scenario (Phase 1: base scenario + per-UE throughput)
 *
 * Derived from the NIST ns3-oran example
 * "oran-lte-2-lte-distance-handover-lm-processing-delay-example.cc".
 * Modified Nov 2026 by H. Zoueihed and N. Sakr: stationary multi-UE two-cell layout,
 * downlink CBR/UDP traffic from a remote host, per-UE throughput logging to CSV,
 * command-line control of load, layout, RIC timing and output files.
 * Original NIST notice: software provided by NIST as a public service, "AS IS",
 * without warranty. Acknowledgement: National Institute of Standards and Technology.
 *
 * Layout (top view, eNB A = cell 1 at x=0, eNB B = cell 2 at x=isd):
 *
 *      near-A UEs (circle r = near-frac*isd)   edge UEs (x = edge-frac*isd)   near-B UEs
 *              (A) ------------------------------- | ----------------------------- (B)
 *
 *   near-A and edge UEs attach to cell A (the cell whose load we raise),
 *   near-B UEs attach to cell B.
 *
 * Outputs:
 *   <out>-tput.csv : one row per UE per window
 *                    time_s,ue,imsi,group,cell_id,rx_bytes,tput_mbps,offered_mbps
 *   <out>-ues.csv  : one row per UE: ue,imsi,group,x,y,initial_cell,offered_mbps
 *   <out>-ho.csv   : one row per completed handover: time_s,imsi,target_cell
 */

#include "ns3/applications-module.h"
#include "ns3/core-module.h"
#include "ns3/internet-module.h"
#include "ns3/lte-module.h"
#include "ns3/mobility-module.h"
#include "ns3/network-module.h"
#include "ns3/oran-module.h"
#include "ns3/point-to-point-module.h"

#include <cmath>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <stdio.h>
#include <vector>

using namespace ns3;

NS_LOG_COMPONENT_DEFINE("XappCoord");

namespace
{
NetDeviceContainer g_ueDevs;
std::vector<uint64_t> g_winBytes; // bytes received by each UE in the current window
std::vector<uint64_t> g_totBytes; // bytes received by each UE after warm-up
std::vector<double> g_offeredMbps;
std::vector<std::string> g_group;
std::ofstream g_tputCsv;
std::ofstream g_hoCsv;
Time g_statsStart;
} // namespace

void
RxPacket(uint32_t ue, Ptr<const Packet> packet)
{
    g_winBytes[ue] += packet->GetSize();
    if (Simulator::Now() >= g_statsStart)
    {
        g_totBytes[ue] += packet->GetSize();
    }
}

uint16_t
ServingCell(uint32_t ue)
{
    return g_ueDevs.Get(ue)->GetObject<LteUeNetDevice>()->GetRrc()->GetCellId();
}

uint64_t
Imsi(uint32_t ue)
{
    return g_ueDevs.Get(ue)->GetObject<LteUeNetDevice>()->GetImsi();
}

void
SampleThroughput(Time window)
{
    double now = Simulator::Now().GetSeconds();
    for (uint32_t i = 0; i < g_ueDevs.GetN(); i++)
    {
        double mbps = g_winBytes[i] * 8.0 / window.GetSeconds() / 1e6;
        g_tputCsv << std::fixed << std::setprecision(3) << now << "," << i << "," << Imsi(i)
                  << "," << g_group[i] << "," << ServingCell(i) << "," << g_winBytes[i] << ","
                  << std::setprecision(4) << mbps << "," << g_offeredMbps[i] << "\n";
        g_winBytes[i] = 0;
    }
    Simulator::Schedule(window, &SampleThroughput, window);
}

void
NotifyHandoverEndOkEnb(std::string context, uint64_t imsi, uint16_t cellid, uint16_t rnti)
{
    std::cout << Simulator::Now().As(Time::S) << " eNB CellId " << cellid
              << ": completed handover of UE with IMSI " << imsi << std::endl;
    g_hoCsv << std::fixed << std::setprecision(4) << Simulator::Now().GetSeconds() << "," << imsi
            << "," << cellid << "\n";
}

void
QueryRcSink(std::string query, std::string args, int rc)
{
    std::cout << Simulator::Now().GetSeconds() << " Query "
              << ((rc == SQLITE_OK || rc == SQLITE_DONE) ? "OK" : "ERROR") << "(" << rc << "): \""
              << query << "\"";
    if (!args.empty())
    {
        std::cout << " (" << args << ")";
    }
    std::cout << std::endl;
}

int
main(int argc, char* argv[])
{
    // ---------------- Layout and radio ----------------
    uint32_t nNearA = 3;   // UEs close to eNB A (cell 1)
    uint32_t nEdge = 3;    // UEs in the overlap zone, attached to A
    uint32_t nNearB = 3;   // UEs close to eNB B (cell 2)
    double isd = 500.0;    // inter-site distance (m)
    double nearFrac = 0.25; // near-UE circle radius as a fraction of isd
    double edgeFrac = 0.45; // x position of edge UEs as a fraction of isd (<0.5 = closer to A)
    double posJitter = 10.0; // +/- uniform jitter on UE x and y (m); changes with RngRun
    double enbTxPower = 30.0; // dBm
    double pathlossExp = 3.0; // log-distance exponent
    uint16_t bandwidthRb = 25; // 25 RBs = 5 MHz
    std::string scheduler = "ns3::PfFfMacScheduler"; // proportional fair (was round robin)

    // ---------------- Traffic ----------------
    double rateA = 1.0;        // offered downlink rate per UE attached to cell A (Mbps)
    double rateB = 1.0;        // offered downlink rate per UE attached to cell B (Mbps)
    uint32_t packetSize = 1024; // bytes
    Time trafficStart = Seconds(3);
    Time warmup = Seconds(2); // statistics start at trafficStart + warmup
    Time window = MilliSeconds(500);
    Time simTime = Seconds(30);

    // ---------------- RIC ----------------
    bool useRic = true;
    bool verbose = false;
    Time maxWaitTime = Seconds(0.010);
    std::string processingDelayRv = "ns3::NormalRandomVariable[Mean=0.005|Variance=0.000031]";
    Time lmQueryInterval = Seconds(1);
    std::string lateCommandPolicy = "DROP";
    bool dbLog = false;

    // ---------------- Output ----------------
    std::string out = "run";
    std::string dbFileName = "oran-repository.db";

    CommandLine cmd(__FILE__);
    cmd.AddValue("n-near-a", "UEs near eNB A (cell 1)", nNearA);
    cmd.AddValue("n-edge", "UEs in the overlap zone, attached to cell 1", nEdge);
    cmd.AddValue("n-near-b", "UEs near eNB B (cell 2)", nNearB);
    cmd.AddValue("isd", "Inter-site distance (m)", isd);
    cmd.AddValue("near-frac", "Near-UE radius as a fraction of isd", nearFrac);
    cmd.AddValue("edge-frac", "Edge-UE x position as a fraction of isd", edgeFrac);
    cmd.AddValue("pos-jitter", "Uniform +/- jitter on UE positions (m)", posJitter);
    cmd.AddValue("enb-tx-power", "eNB transmit power (dBm)", enbTxPower);
    cmd.AddValue("pathloss-exp", "Log-distance pathloss exponent", pathlossExp);
    cmd.AddValue("bandwidth-rb", "DL/UL bandwidth in resource blocks", bandwidthRb);
    cmd.AddValue("scheduler", "MAC scheduler type, e.g. ns3::PfFfMacScheduler", scheduler);
    cmd.AddValue("rate-a", "Offered DL rate per UE in cell 1 (Mbps)", rateA);
    cmd.AddValue("rate-b", "Offered DL rate per UE in cell 2 (Mbps)", rateB);
    cmd.AddValue("packet-size", "UDP packet size (bytes)", packetSize);
    cmd.AddValue("traffic-start", "Time traffic starts", trafficStart);
    cmd.AddValue("warmup", "Warm-up after traffic start, excluded from the summary", warmup);
    cmd.AddValue("window", "Throughput measurement window", window);
    cmd.AddValue("sim-time", "The amount of time to simulate", simTime);
    cmd.AddValue("ric", "Enable the Near-RT RIC", useRic);
    cmd.AddValue("verbose", "Verbose ORAN helper output", verbose);
    cmd.AddValue("max-wait-time", "LM deadline (0 = no deadline)", maxWaitTime);
    cmd.AddValue("processing-delay-rv", "LM processing delay random variable", processingDelayRv);
    cmd.AddValue("lm-query-interval", "Control period of the RIC", lmQueryInterval);
    cmd.AddValue("late-command-policy", "\"DROP\" or \"SAVE\"", lateCommandPolicy);
    cmd.AddValue("db-log", "Print SQL query results", dbLog);
    cmd.AddValue("out", "Prefix for CSV output files", out);
    cmd.AddValue("db-file", "SQLite repository file (unique per parallel run)", dbFileName);
    cmd.Parse(argc, argv);

    LogComponentEnable("OranNearRtRic", (LogLevel)(LOG_PREFIX_TIME | LOG_WARN));

    Config::SetDefault("ns3::LteHelper::UseIdealRrc", BooleanValue(false));
    Config::SetDefault("ns3::LteEnbPhy::TxPower", DoubleValue(enbTxPower));
    Config::SetDefault("ns3::LteEnbNetDevice::DlBandwidth", UintegerValue(bandwidthRb));
    Config::SetDefault("ns3::LteEnbNetDevice::UlBandwidth", UintegerValue(bandwidthRb));

    // ---------------- LTE + EPC ----------------
    Ptr<LteHelper> lteHelper = CreateObject<LteHelper>();
    Ptr<PointToPointEpcHelper> epcHelper = CreateObject<PointToPointEpcHelper>();
    lteHelper->SetEpcHelper(epcHelper);
    lteHelper->SetSchedulerType(scheduler);
    lteHelper->SetHandoverAlgorithmType("ns3::NoOpHandoverAlgorithm"); // only the RIC hands over
    lteHelper->SetAttribute("PathlossModel", StringValue("ns3::LogDistancePropagationLossModel"));
    lteHelper->SetPathlossModelAttribute("Exponent", DoubleValue(pathlossExp));

    Ptr<Node> pgw = epcHelper->GetPgwNode();

    // Remote host that sends the downlink traffic
    NodeContainer remoteHostContainer;
    remoteHostContainer.Create(1);
    Ptr<Node> remoteHost = remoteHostContainer.Get(0);
    InternetStackHelper internet;
    internet.Install(remoteHostContainer);

    PointToPointHelper p2ph;
    p2ph.SetDeviceAttribute("DataRate", DataRateValue(DataRate("100Gb/s")));
    p2ph.SetDeviceAttribute("Mtu", UintegerValue(1500));
    p2ph.SetChannelAttribute("Delay", TimeValue(MilliSeconds(10)));
    NetDeviceContainer internetDevices = p2ph.Install(pgw, remoteHost);
    Ipv4AddressHelper ipv4h;
    ipv4h.SetBase("1.0.0.0", "255.0.0.0");
    ipv4h.Assign(internetDevices);

    Ipv4StaticRoutingHelper ipv4RoutingHelper;
    Ptr<Ipv4StaticRouting> remoteHostStaticRouting =
        ipv4RoutingHelper.GetStaticRouting(remoteHost->GetObject<Ipv4>());
    remoteHostStaticRouting->AddNetworkRouteTo(Ipv4Address("7.0.0.0"), Ipv4Mask("255.0.0.0"), 1);

    // ---------------- Nodes and positions ----------------
    uint32_t numberOfUes = nNearA + nEdge + nNearB;
    NodeContainer enbNodes;
    NodeContainer ueNodes;
    enbNodes.Create(2);
    ueNodes.Create(numberOfUes);

    MobilityHelper mobility;
    mobility.SetMobilityModel("ns3::ConstantPositionMobilityModel");
    mobility.Install(enbNodes);
    mobility.Install(ueNodes);
    enbNodes.Get(0)->GetObject<MobilityModel>()->SetPosition(Vector(0, 0, 30));
    enbNodes.Get(1)->GetObject<MobilityModel>()->SetPosition(Vector(isd, 0, 30));

    Ptr<UniformRandomVariable> jitter = CreateObject<UniformRandomVariable>();
    jitter->SetAttribute("Min", DoubleValue(-posJitter));
    jitter->SetAttribute("Max", DoubleValue(posJitter));

    // UE order: near-A [0, nNearA), edge [nNearA, nNearA+nEdge), near-B [rest]
    std::vector<Vector> uePos;
    std::vector<uint32_t> ueEnb; // index of the eNB each UE attaches to initially
    double r = nearFrac * isd;
    for (uint32_t k = 0; k < nNearA; k++)
    {
        double a = 2 * M_PI * (k + 0.5) / nNearA;
        uePos.emplace_back(r * std::cos(a), r * std::sin(a), 1.5);
        ueEnb.push_back(0);
        g_group.emplace_back("nearA");
    }
    for (uint32_t k = 0; k < nEdge; k++)
    {
        double spread = 0.3 * isd;
        double y = (nEdge > 1) ? (-spread / 2 + spread * k / (nEdge - 1)) : 0.0;
        uePos.emplace_back(edgeFrac * isd, y, 1.5);
        ueEnb.push_back(0);
        g_group.emplace_back("edge");
    }
    for (uint32_t k = 0; k < nNearB; k++)
    {
        double a = 2 * M_PI * (k + 0.5) / nNearB;
        uePos.emplace_back(isd + r * std::cos(a), r * std::sin(a), 1.5);
        ueEnb.push_back(1);
        g_group.emplace_back("nearB");
    }
    for (uint32_t i = 0; i < numberOfUes; i++)
    {
        Vector p = uePos[i];
        p.x += jitter->GetValue();
        p.y += jitter->GetValue();
        uePos[i] = p;
        ueNodes.Get(i)->GetObject<MobilityModel>()->SetPosition(p);
    }

    // ---------------- LTE devices, IP, attach ----------------
    NetDeviceContainer enbLteDevs = lteHelper->InstallEnbDevice(enbNodes);
    NetDeviceContainer ueLteDevs = lteHelper->InstallUeDevice(ueNodes);
    g_ueDevs = ueLteDevs;

    internet.Install(ueNodes);
    Ipv4InterfaceContainer ueIpIfaces = epcHelper->AssignUeIpv4Address(NetDeviceContainer(ueLteDevs));
    for (uint32_t i = 0; i < numberOfUes; i++)
    {
        Ptr<Ipv4StaticRouting> ueStaticRouting =
            ipv4RoutingHelper.GetStaticRouting(ueNodes.Get(i)->GetObject<Ipv4>());
        ueStaticRouting->SetDefaultRoute(epcHelper->GetUeDefaultGatewayAddress(), 1);
        lteHelper->Attach(ueLteDevs.Get(i), enbLteDevs.Get(ueEnb[i]));
    }
    lteHelper->AddX2Interface(enbNodes);

    // ---------------- Traffic: CBR/UDP downlink, one flow per UE ----------------
    g_winBytes.assign(numberOfUes, 0);
    g_totBytes.assign(numberOfUes, 0);
    g_offeredMbps.resize(numberOfUes);
    g_statsStart = trafficStart + warmup;

    Ptr<UniformRandomVariable> startJitter = CreateObject<UniformRandomVariable>();
    startJitter->SetAttribute("Min", DoubleValue(0.0));
    startJitter->SetAttribute("Max", DoubleValue(0.1));

    uint16_t basePort = 10000;
    for (uint32_t i = 0; i < numberOfUes; i++)
    {
        g_offeredMbps[i] = (ueEnb[i] == 0) ? rateA : rateB;

        UdpServerHelper server(basePort + i);
        ApplicationContainer serverApp = server.Install(ueNodes.Get(i));
        serverApp.Start(Seconds(0.5));
        serverApp.Stop(simTime);
        DynamicCast<UdpServer>(serverApp.Get(0))
            ->TraceConnectWithoutContext("Rx", MakeBoundCallback(&RxPacket, i));

        if (g_offeredMbps[i] <= 0)
        {
            continue;
        }
        UdpClientHelper client(ueIpIfaces.GetAddress(i), basePort + i);
        client.SetAttribute("Interval",
                            TimeValue(Seconds(packetSize * 8.0 / (g_offeredMbps[i] * 1e6))));
        client.SetAttribute("PacketSize", UintegerValue(packetSize));
        client.SetAttribute("MaxPackets", UintegerValue(std::numeric_limits<uint32_t>::max()));
        ApplicationContainer clientApp = client.Install(remoteHost);
        clientApp.Start(trafficStart + Seconds(startJitter->GetValue()));
        clientApp.Stop(simTime);
    }

    // ---------------- Output files ----------------
    g_tputCsv.open(out + "-tput.csv");
    g_tputCsv << "time_s,ue,imsi,group,cell_id,rx_bytes,tput_mbps,offered_mbps\n";
    g_hoCsv.open(out + "-ho.csv");
    g_hoCsv << "time_s,imsi,target_cell\n";
    {
        std::ofstream ues(out + "-ues.csv");
        ues << "ue,imsi,group,x,y,initial_cell,offered_mbps\n";
        for (uint32_t i = 0; i < numberOfUes; i++)
        {
            ues << i << "," << Imsi(i) << "," << g_group[i] << "," << uePos[i].x << ","
                << uePos[i].y << "," << (ueEnb[i] + 1) << "," << g_offeredMbps[i] << "\n";
        }
    }
    Simulator::Schedule(trafficStart + window, &SampleThroughput, window);

    // ---------------- ORAN models ----------------
    if (useRic)
    {
        Ptr<OranNearRtRic> nearRtRic = nullptr;
        OranE2NodeTerminatorContainer e2NodeTerminatorsEnbs;
        OranE2NodeTerminatorContainer e2NodeTerminatorsUes;
        Ptr<OranHelper> oranHelper = CreateObject<OranHelper>();

        oranHelper->SetAttribute("Verbose", BooleanValue(verbose));
        oranHelper->SetAttribute("LmQueryInterval", TimeValue(lmQueryInterval));
        oranHelper->SetAttribute("E2NodeInactivityThreshold", TimeValue(Seconds(2)));
        oranHelper->SetAttribute("E2NodeInactivityIntervalRv",
                                 StringValue("ns3::ConstantRandomVariable[Constant=2]"));
        oranHelper->SetAttribute("LmQueryMaxWaitTime", TimeValue(maxWaitTime));
        oranHelper->SetAttribute("LmQueryLateCommandPolicy", StringValue(lateCommandPolicy));

        if (!dbFileName.empty())
        {
            std::remove(dbFileName.c_str());
        }
        oranHelper->SetDataRepository("ns3::OranDataRepositorySqlite",
                                      "DatabaseFile",
                                      StringValue(dbFileName));
        // Placeholder xApp for Phase 1: hands a UE to its closest eNB.
        // Replaced by the TS and TPC xApps in Phase 3.
        oranHelper->SetDefaultLogicModule("ns3::OranLmLte2LteDistanceHandover",
                                          "ProcessingDelayRv",
                                          StringValue(processingDelayRv));
        oranHelper->SetConflictMitigationModule("ns3::OranCmmNoop");

        nearRtRic = oranHelper->CreateNearRtRic();

        oranHelper->SetE2NodeTerminator("ns3::OranE2NodeTerminatorLteUe",
                                        "RegistrationIntervalRv",
                                        StringValue("ns3::ConstantRandomVariable[Constant=1]"),
                                        "SendIntervalRv",
                                        StringValue("ns3::ConstantRandomVariable[Constant=1]"));
        oranHelper->AddReporter("ns3::OranReporterLocation",
                                "Trigger",
                                StringValue("ns3::OranReportTriggerPeriodic"));
        oranHelper->AddReporter(
            "ns3::OranReporterLteUeCellInfo",
            "Trigger",
            StringValue("ns3::OranReportTriggerLteUeHandover[InitialReport=true]"));
        e2NodeTerminatorsUes.Add(oranHelper->DeployTerminators(nearRtRic, ueNodes));

        oranHelper->SetE2NodeTerminator("ns3::OranE2NodeTerminatorLteEnb",
                                        "RegistrationIntervalRv",
                                        StringValue("ns3::ConstantRandomVariable[Constant=1]"),
                                        "SendIntervalRv",
                                        StringValue("ns3::ConstantRandomVariable[Constant=1]"));
        oranHelper->AddReporter("ns3::OranReporterLocation",
                                "Trigger",
                                StringValue("ns3::OranReportTriggerPeriodic"));
        e2NodeTerminatorsEnbs.Add(oranHelper->DeployTerminators(nearRtRic, enbNodes));

        if (dbLog)
        {
            nearRtRic->Data()->TraceConnectWithoutContext("QueryRc", MakeCallback(&QueryRcSink));
        }

        Simulator::Schedule(Seconds(1), &OranHelper::ActivateAndStartNearRtRic, oranHelper, nearRtRic);
        Simulator::Schedule(Seconds(1.5),
                            &OranHelper::ActivateE2NodeTerminators,
                            oranHelper,
                            e2NodeTerminatorsEnbs);
        Simulator::Schedule(Seconds(2),
                            &OranHelper::ActivateE2NodeTerminators,
                            oranHelper,
                            e2NodeTerminatorsUes);
    }

    Config::Connect("/NodeList/*/DeviceList/*/LteEnbRrc/HandoverEndOk",
                    MakeCallback(&NotifyHandoverEndOkEnb));

    Simulator::Stop(simTime);
    Simulator::Run();

    // ---------------- Summary (after warm-up) ----------------
    double statsSec = (simTime - g_statsStart).GetSeconds();
    std::map<uint16_t, double> cellOffered;
    std::map<uint16_t, double> cellDelivered;
    std::cout << "\nUE  group  cell  offered_Mbps  delivered_Mbps\n";
    for (uint32_t i = 0; i < numberOfUes; i++)
    {
        double delivered = g_totBytes[i] * 8.0 / statsSec / 1e6;
        uint16_t cell = ServingCell(i);
        cellOffered[cell] += g_offeredMbps[i];
        cellDelivered[cell] += delivered;
        std::cout << std::setw(2) << i << "  " << std::setw(5) << g_group[i] << "  " << std::setw(4)
                  << cell << "  " << std::setw(12) << std::fixed << std::setprecision(3)
                  << g_offeredMbps[i] << "  " << std::setw(14) << delivered << "\n";
    }
    std::cout << "\nCell  offered_Mbps  delivered_Mbps\n";
    for (auto& [cell, offered] : cellOffered)
    {
        std::cout << std::setw(4) << cell << "  " << std::setw(12) << offered << "  "
                  << std::setw(14) << cellDelivered[cell] << "\n";
    }

    g_tputCsv.close();
    g_hoCsv.close();
    Simulator::Destroy();
    return 0;
}
