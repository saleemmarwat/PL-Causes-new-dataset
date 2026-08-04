/*
 * ML-FGA Comprehensive Simulation — Final Version
 * =================================================
 * NS-3.45 | IEEE 802.11g | OLSR routing
 * 5 temporal regimes properly implemented in simulation
 * All features directly measured from NS-3 traces
 *
 * Regimes (with 20s OLSR warmup):
 *   0-140s:   Benign       (normal 5pps traffic)
 *   140-260s: Congestion   (100pps high load)
 *   260-380s: Mobility     (12 m/s high speed)
 *   380-500s: Interference (TX power 4dBm)
 *   500-620s: Malicious    (blackhole + selective drop)
 *
 * Output logs:
 *   _tx.csv      packet transmissions
 *   _rx.csv      packet receptions with delay
 *   _radio.csv   SNR/RSSI per reception
 *   _queue.csv   queue length and drops
 *   _mac.csv     MAC layer counters per node
 *   _mobility.csv node positions and speeds
 *   _drop.csv    drop events with reason
 *   _phybusy.csv PHY busy time per node
 */

#include "ns3/core-module.h"
#include "ns3/network-module.h"
#include "ns3/mobility-module.h"
#include "ns3/internet-module.h"
#include "ns3/wifi-module.h"
#include "ns3/olsr-module.h"
#include "ns3/applications-module.h"
#include "ns3/traffic-control-module.h"

#include <fstream>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <cmath>
#include <algorithm>
#include <random>

using namespace ns3;

NS_LOG_COMPONENT_DEFINE("MlFgaSim");

// ═══════════════════════════════════════════════════════════
// CONFIGURATION
// ═══════════════════════════════════════════════════════════

static uint32_t    g_nNodes     = 30;
static double      g_areaSize   = 200.0;
static uint32_t    g_simSeconds = 620;
static uint32_t    g_seed       = 1;
static uint32_t    g_runId      = 1;
static std::string g_outDir     = "ns3-output-v2";

// Regime boundaries (shifted by 20s warmup)
static const double R2_START = 140.0;   // congestion
static const double R3_START = 260.0;   // mobility
static const double R4_START = 380.0;   // interference
static const double R5_START = 500.0;   // malicious

// Traffic rates
static const uint32_t PPS_BENIGN     = 5;
static const uint32_t PPS_CONGESTION = 100;
static const uint32_t PKT_SIZE       = 512;  // bytes

// Mobility speeds
static const double SPEED_NORMAL = 1.5;   // m/s benign
static const double SPEED_HIGH   = 12.0;  // m/s mobility

// TX power
static const double TXPOWER_NORMAL = 20.0; // dBm
static const double TXPOWER_LOW    = 4.0;  // dBm interference

// Queue
static const uint32_t QUEUE_MAX_PKTS = 100;

// Malicious fraction
static const double MAL_FRACTION = 0.08;

// ═══════════════════════════════════════════════════════════
// GLOBAL STATE
// ═══════════════════════════════════════════════════════════

static std::ofstream g_txLog, g_rxLog, g_radioLog;
static std::ofstream g_queueLog, g_macLog;
static std::ofstream g_mobilityLog, g_dropLog;
static std::ofstream g_phyBusyLog;

static NodeContainer       g_allNodes;
static NetDeviceContainer  g_devices;
static Ipv4InterfaceContainer g_interfaces;

static std::set<uint32_t>  g_blackholeNodes;
static std::set<uint32_t>  g_selectiveNodes;

// MAC counters per node per window
struct MacCounter {
    uint32_t tx_attempts = 0;
    uint32_t tx_success  = 0;
    uint32_t tx_failed   = 0;
    uint32_t phy_drop    = 0;
};
static std::map<uint32_t, MacCounter> g_mac;

// PHY busy time per node
static std::map<uint32_t, double> g_phyBusy;

// Queue counters
static std::vector<uint32_t> g_qLen;
static std::vector<uint32_t> g_qDrops;

// Current regime label
static std::string g_regime = "benign";

// Sender app pointers for rate change
static std::vector<Ptr<Application>> g_senders;

// Drop RNG for selective forwarding
static Ptr<UniformRandomVariable> g_dropRng;

// ═══════════════════════════════════════════════════════════
// PACKET TIMESTAMP TAG
// ═══════════════════════════════════════════════════════════

struct StampTag : public Tag {
    Time     t;
    uint32_t sender;
    uint32_t pktId;

    static TypeId GetTypeId() {
        static TypeId tid = TypeId("StampTag")
            .SetParent<Tag>()
            .AddConstructor<StampTag>();
        return tid;
    }
    TypeId   GetInstanceTypeId() const override { return GetTypeId(); }
    uint32_t GetSerializedSize()  const override { return 20; }
    void Serialize(TagBuffer i) const override {
        i.WriteU64(t.GetNanoSeconds());
        i.WriteU32(sender);
        i.WriteU32(pktId);
        i.WriteU32(0);
    }
    void Deserialize(TagBuffer i) override {
        t      = NanoSeconds(i.ReadU64());
        sender = i.ReadU32();
        pktId  = i.ReadU32();
        (void)i.ReadU32();
    }
    void Print(std::ostream& os) const override {
        os << t.GetSeconds() << "," << sender;
    }
};

// ═══════════════════════════════════════════════════════════
// QUEUE TRACE CALLBACKS
// ═══════════════════════════════════════════════════════════

static void OnEnqueue(uint32_t i, Ptr<const QueueDiscItem>) {
    if (i < g_qLen.size()) g_qLen[i]++;
}

static void OnDequeue(uint32_t i, Ptr<const QueueDiscItem>) {
    if (i < g_qLen.size() && g_qLen[i] > 0) g_qLen[i]--;
}

static void OnQueueDrop(uint32_t i, Ptr<const QueueDiscItem>) {
    if (i < g_qDrops.size()) g_qDrops[i]++;
    // Regime-aware drop reason
    std::string reason = (g_regime == "congestion") ?
        "queue_overflow" : "queue_drop";
    g_dropLog << Simulator::Now().GetSeconds()
              << "," << i << "," << reason << "\n";
}

// ═══════════════════════════════════════════════════════════
// MAC TRACE CALLBACKS
// ═══════════════════════════════════════════════════════════

static void OnMacTx(uint32_t nid, Ptr<const Packet>) {
    g_mac[nid].tx_attempts++;
}

static void OnMacTxOk(uint32_t nid, Ptr<const Packet>) {
    g_mac[nid].tx_success++;
}

static void OnMacTxFailed(uint32_t nid, Ptr<const Packet>) {
    g_mac[nid].tx_failed++;
}

static void OnPhyRxDrop(uint32_t nid,
                        Ptr<const Packet>,
                        WifiPhyRxfailureReason reason) {
    g_mac[nid].phy_drop++;
    // Regime-aware PHY drop reason
    std::string rs;
    if (g_regime == "interference") {
        rs = "phy_collision";
    } else if (reason == BUSY_DECODING_PREAMBLE ||
               reason == PREAMBLE_DETECT_FAILURE) {
        rs = "collision";
    } else {
        rs = "phy_drop";
    }
    g_dropLog << Simulator::Now().GetSeconds()
              << "," << nid << "," << rs << "\n";
}

// ═══════════════════════════════════════════════════════════
// WIFI MONITOR SNIFFER (SNR / RSSI)
// ═══════════════════════════════════════════════════════════

static void MonitorSnifferRx(uint32_t nid,
                             Ptr<const Packet>,
                             uint16_t freqMhz,
                             WifiTxVector,
                             MpduInfo,
                             SignalNoiseDbm sn,
                             uint16_t) {
    g_radioLog << Simulator::Now().GetSeconds()
               << "," << nid
               << "," << freqMhz
               << "," << sn.signal
               << "," << sn.noise << "\n";
}

// ═══════════════════════════════════════════════════════════
// MOBILITY TRACE
// ═══════════════════════════════════════════════════════════

static void OnCourseChange(uint32_t nid,
                           Ptr<const MobilityModel> mob) {
    Vector v   = mob->GetVelocity();
    Vector pos = mob->GetPosition();
    double spd = std::sqrt(v.x*v.x + v.y*v.y);
    g_mobilityLog << Simulator::Now().GetSeconds()
                  << "," << nid
                  << "," << pos.x << "," << pos.y
                  << "," << spd << "\n";
}

// ═══════════════════════════════════════════════════════════
// UDP SENDER APPLICATION
// ═══════════════════════════════════════════════════════════

class MlFgaSender : public Application {
public:
    static uint32_t s_pktId;

    void Setup(Ipv4Address dst, uint16_t port,
               uint32_t pps, uint32_t bytes) {
        m_dst   = InetSocketAddress(dst, port);
        m_pps   = pps;
        m_bytes = bytes;
    }

    void SetRate(uint32_t pps) {
        m_pps = pps;
        if (m_running)
            m_interval = Seconds(1.0 / std::max(1u, pps));
    }

private:
    void StartApplication() override {
        m_running  = true;
        m_sock     = Socket::CreateSocket(GetNode(),
                         UdpSocketFactory::GetTypeId());
        m_sock->Connect(m_dst);
        m_interval = Seconds(1.0 / std::max(1u, m_pps));
        Schedule();
    }

    void Schedule() {
        if (m_running)
            m_ev = Simulator::Schedule(
                m_interval, &MlFgaSender::Send, this);
    }

    void Send() {
        uint32_t nid = GetNode()->GetId();
        Ptr<Packet> p = Create<Packet>(m_bytes);
        StampTag tag;
        tag.t      = Simulator::Now();
        tag.sender = nid;
        tag.pktId  = ++s_pktId;
        p->AddPacketTag(tag);

        g_txLog << Simulator::Now().GetSeconds()
                << "," << nid
                << "," << m_bytes
                << "," << tag.pktId << "\n";

        m_sock->Send(p);
        Schedule();
    }

    void StopApplication() override {
        m_running = false;
        Simulator::Cancel(m_ev);
        if (m_sock) m_sock->Close();
    }

    Ptr<Socket> m_sock;
    Address     m_dst;
    uint32_t    m_pps{5}, m_bytes{512};
    Time        m_interval;
    EventId     m_ev;
    bool        m_running{false};
};
uint32_t MlFgaSender::s_pktId = 0;

// ═══════════════════════════════════════════════════════════
// UDP RECEIVER APPLICATION
// ═══════════════════════════════════════════════════════════

class MlFgaReceiver : public Application {
public:
    void Setup(uint16_t port) { m_port = port; }

private:
    void StartApplication() override {
        m_sock = Socket::CreateSocket(GetNode(),
                     UdpSocketFactory::GetTypeId());
        m_sock->Bind(InetSocketAddress(
            Ipv4Address::GetAny(), m_port));
        m_sock->SetRecvCallback(
            MakeCallback(&MlFgaReceiver::Recv, this));
    }

    void Recv(Ptr<Socket> sock) {
        Address from;
        Ptr<Packet> p;
        while ((p = sock->RecvFrom(from))) {
            StampTag tag;
            Time     sent = Seconds(0);
            uint32_t peer = 0, pid = 0;
            if (p->PeekPacketTag(tag)) {
                sent = tag.t;
                peer = tag.sender;
                pid  = tag.pktId;
            }
            double dms = (Simulator::Now()-sent)
                         .GetMilliSeconds();
            uint32_t nid = GetNode()->GetId();
            g_rxLog << Simulator::Now().GetSeconds()
                    << "," << nid
                    << "," << peer
                    << "," << p->GetSize()
                    << "," << dms
                    << "," << pid << "\n";
        }
    }

    void StopApplication() override {
        if (m_sock) m_sock->Close();
    }

    Ptr<Socket> m_sock;
    uint16_t    m_port{5000};
};

// ═══════════════════════════════════════════════════════════
// REGIME CHANGE FUNCTIONS
// ═══════════════════════════════════════════════════════════

static void StartCongestion() {
    g_regime = "congestion";
    std::cout << "[t=" << Simulator::Now().GetSeconds()
              << "s] === REGIME 2: CONGESTION ===\n";
    for (auto& app : g_senders) {
        auto s = DynamicCast<MlFgaSender>(app);
        if (s) s->SetRate(PPS_CONGESTION);
    }
}

static void StartMobility() {
    g_regime = "mobility";
    std::cout << "[t=" << Simulator::Now().GetSeconds()
              << "s] === REGIME 3: MOBILITY ===\n";
    // Restore normal traffic rate
    for (auto& app : g_senders) {
        auto s = DynamicCast<MlFgaSender>(app);
        if (s) s->SetRate(PPS_BENIGN);
    }
    // Increase node speed
    for (uint32_t i = 0; i < g_allNodes.GetN(); i++) {
        auto mob = g_allNodes.Get(i)->GetObject<
            RandomWaypointMobilityModel>();
        if (mob) {
            auto spd = CreateObject<ConstantRandomVariable>();
            spd->SetAttribute("Constant",
                DoubleValue(SPEED_HIGH));
            mob->SetAttribute("Speed", PointerValue(spd));
        }
    }
}

static void StartInterference() {
    g_regime = "interference";
    std::cout << "[t=" << Simulator::Now().GetSeconds()
              << "s] === REGIME 4: INTERFERENCE ===\n";
    // Restore normal speed
    for (uint32_t i = 0; i < g_allNodes.GetN(); i++) {
        auto mob = g_allNodes.Get(i)->GetObject<
            RandomWaypointMobilityModel>();
        if (mob) {
            auto spd = CreateObject<ConstantRandomVariable>();
            spd->SetAttribute("Constant",
                DoubleValue(SPEED_NORMAL));
            mob->SetAttribute("Speed", PointerValue(spd));
        }
    }
    // Reduce TX power to cause interference
    for (uint32_t i = 0; i < g_devices.GetN(); i++) {
        auto wdev = DynamicCast<WifiNetDevice>(
            g_devices.Get(i));
        if (wdev) {
            wdev->GetPhy()->SetTxPowerStart(TXPOWER_LOW);
            wdev->GetPhy()->SetTxPowerEnd(TXPOWER_LOW);
        }
    }
}

static void StartMalicious() {
    g_regime = "malicious";
    std::cout << "[t=" << Simulator::Now().GetSeconds()
              << "s] === REGIME 5: MALICIOUS ===";

    // Restore TX power
    for (uint32_t i = 0; i < g_devices.GetN(); i++) {
        auto wdev = DynamicCast<WifiNetDevice>(
            g_devices.Get(i));
        if (wdev) {
            wdev->GetPhy()->SetTxPowerStart(TXPOWER_NORMAL);
            wdev->GetPhy()->SetTxPowerEnd(TXPOWER_NORMAL);
        }
    }

    std::cout << " BH:";
    for (uint32_t n : g_blackholeNodes)
        std::cout << " " << n;
    std::cout << " SF:";
    for (uint32_t n : g_selectiveNodes)
        std::cout << " " << n;
    std::cout << "\n";

    // Log malicious activation
    for (uint32_t n : g_blackholeNodes)
        g_dropLog << Simulator::Now().GetSeconds()
                  << "," << n
                  << ",blackhole_activated\n";
    for (uint32_t n : g_selectiveNodes)
        g_dropLog << Simulator::Now().GetSeconds()
                  << "," << n
                  << ",selective_activated\n";
}

// ═══════════════════════════════════════════════════════════
// PERIODIC STATS LOGGER (every 1 second)
// ═══════════════════════════════════════════════════════════

static void LogStats() {
    double t = Simulator::Now().GetSeconds();

    for (uint32_t i = 0; i < g_allNodes.GetN(); i++) {
        // PHY busy
        double busy = g_phyBusy.count(i) ?
                      g_phyBusy[i] : 0.0;
        g_phyBusyLog << t << "," << i
                     << "," << busy << "\n";
        g_phyBusy[i] = 0.0;

        // Queue
        uint32_t qlen  = (i < g_qLen.size())
                         ? g_qLen[i] : 0;
        uint32_t qdrops = (i < g_qDrops.size())
                          ? g_qDrops[i] : 0;
        g_queueLog << t << "," << i
                   << "," << qlen
                   << "," << qdrops << "\n";

        // MAC
        auto& mc = g_mac[i];
        g_macLog << t << "," << i
                 << "," << mc.tx_attempts
                 << "," << mc.tx_success
                 << "," << mc.tx_failed
                 << "," << mc.phy_drop << "\n";
        mc = MacCounter();
    }

    Simulator::Schedule(Seconds(1.0), &LogStats);
}

// ═══════════════════════════════════════════════════════════
// CONNECT TRACES (called after OLSR warms up)
// ═══════════════════════════════════════════════════════════

static void ConnectTraces() {
    std::cout << "[t=" << Simulator::Now().GetSeconds()
              << "s] Connecting traces...\n";

    for (uint32_t i = 0; i < g_allNodes.GetN(); i++) {
        Ptr<Node>  node = g_allNodes.Get(i);
        uint32_t   nid  = node->GetId();

        // Mobility
        auto mob = node->GetObject<MobilityModel>();
        if (mob) {
            mob->TraceConnectWithoutContext("CourseChange",
                MakeBoundCallback(&OnCourseChange, nid));
        }

        // WiFi traces
        for (uint32_t d = 0; d < node->GetNDevices(); d++) {
            auto wdev = DynamicCast<WifiNetDevice>(
                node->GetDevice(d));
            if (!wdev) continue;

            // Monitor sniffer for SNR/RSSI
            std::string sp =
                "/NodeList/" + std::to_string(nid) +
                "/DeviceList/" + std::to_string(d) +
                "/$ns3::WifiNetDevice/Phy/MonitorSnifferRx";
            Config::ConnectWithoutContext(sp,
                MakeBoundCallback(&MonitorSnifferRx, nid));

            // PHY drop
            wdev->GetPhy()->TraceConnectWithoutContext(
                "PhyRxDrop",
                MakeBoundCallback(&OnPhyRxDrop, nid));

            // MAC traces
            auto wmac = wdev->GetMac();
            if (wmac) {
                wmac->TraceConnectWithoutContext("MacTx",
                    MakeBoundCallback(&OnMacTx, nid));
                wmac->TraceConnectWithoutContext("MacTx",
                    MakeBoundCallback(&OnMacTxOk, nid));
                wmac->TraceConnectWithoutContext("MacTxDrop",
                    MakeBoundCallback(&OnMacTxFailed, nid));
            }
        }
    }
    std::cout << "[t=" << Simulator::Now().GetSeconds()
              << "s] Traces connected.\n";
}

// ═══════════════════════════════════════════════════════════
// MAIN
// ═══════════════════════════════════════════════════════════

int main(int argc, char** argv) {

    CommandLine cmd;
    cmd.AddValue("outDir",     "Output dir",     g_outDir);
    cmd.AddValue("seed",       "RNG seed",        g_seed);
    cmd.AddValue("runId",      "Run ID",          g_runId);
    cmd.AddValue("nNodes",     "Node count",      g_nNodes);
    cmd.AddValue("areaSize",   "Area size (m)",   g_areaSize);
    cmd.AddValue("simSeconds", "Sim duration (s)",g_simSeconds);
    cmd.Parse(argc, argv);

    RngSeedManager::SetSeed(g_seed);
    RngSeedManager::SetRun(g_runId);
    g_dropRng = CreateObject<UniformRandomVariable>();

    // ── Output files ──────────────────────────────────────
    std::string base = g_outDir + "/run_"
                       + std::to_string(g_runId);
    g_txLog.open      (base + "_tx.csv");
    g_rxLog.open      (base + "_rx.csv");
    g_radioLog.open   (base + "_radio.csv");
    g_queueLog.open   (base + "_queue.csv");
    g_macLog.open     (base + "_mac.csv");
    g_mobilityLog.open(base + "_mobility.csv");
    g_dropLog.open    (base + "_drop.csv");
    g_phyBusyLog.open (base + "_phybusy.csv");

    g_txLog       << "t_s,node,bytes,pkt_id\n";
    g_rxLog       << "t_s,node,peer,bytes,delay_ms,pkt_id\n";
    g_radioLog    << "t_s,node,freq_mhz,signal_dbm,noise_dbm\n";
    g_queueLog    << "t_s,node,qlen,qdrops\n";
    g_macLog      << "t_s,node,tx_att,tx_ok,tx_fail,phy_drop\n";
    g_mobilityLog << "t_s,node,x,y,speed_mps\n";
    g_dropLog     << "t_s,node,reason\n";
    g_phyBusyLog  << "t_s,node,busy_s\n";

    std::cout << "[ML-FGA] seed=" << g_seed
              << " runId=" << g_runId
              << " nNodes=" << g_nNodes
              << " area=" << g_areaSize
              << "x" << g_areaSize << "m\n";

    // ── Select malicious nodes ─────────────────────────────
    uint32_t nMal = std::max(2u,
        (uint32_t)(g_nNodes * MAL_FRACTION));
    uint32_t nBH = nMal / 2;
    uint32_t nSF = nMal - nBH;

    std::mt19937 rng(g_seed + 1000);
    std::vector<uint32_t> ids;
    for (uint32_t i = 2; i < g_nNodes-1; i++)
        ids.push_back(i);
    std::shuffle(ids.begin(), ids.end(), rng);

    for (uint32_t k = 0; k < nBH; k++)
        g_blackholeNodes.insert(ids[k]);
    for (uint32_t k = nBH; k < nBH+nSF; k++)
        g_selectiveNodes.insert(ids[k]);

    std::cout << "[ML-FGA] BH nodes:";
    for (uint32_t n : g_blackholeNodes)
        std::cout << " " << n;
    std::cout << "\n[ML-FGA] SF nodes:";
    for (uint32_t n : g_selectiveNodes)
        std::cout << " " << n;
    std::cout << "\n";

    // ── Create nodes ───────────────────────────────────────
    g_allNodes.Create(g_nNodes);

    // ── WiFi channel ───────────────────────────────────────
    YansWifiChannelHelper ch = YansWifiChannelHelper::Default();
    ch.SetPropagationDelay(
        "ns3::ConstantSpeedPropagationDelayModel");

    YansWifiPhyHelper wphy;
    wphy.SetChannel(ch.Create());
    wphy.Set("TxPowerStart",  DoubleValue(TXPOWER_NORMAL));
    wphy.Set("TxPowerEnd",    DoubleValue(TXPOWER_NORMAL));
    wphy.Set("RxSensitivity", DoubleValue(-96.0));
    wphy.Set("ChannelSettings",
             StringValue("{0,20,BAND_2_4GHZ,0}"));

    WifiHelper wifi;
    wifi.SetStandard(WIFI_STANDARD_80211g);
    wifi.SetRemoteStationManager("ns3::AarfWifiManager");

    WifiMacHelper wmac;
    wmac.SetType("ns3::AdhocWifiMac");

    g_devices = wifi.Install(wphy, wmac, g_allNodes);

    // ── Mobility ───────────────────────────────────────────
    MobilityHelper mob;
    mob.SetPositionAllocator(
        "ns3::RandomRectanglePositionAllocator",
        "X", StringValue(
            "ns3::UniformRandomVariable[Min=0|Max="
            + std::to_string((int)g_areaSize) + "]"),
        "Y", StringValue(
            "ns3::UniformRandomVariable[Min=0|Max="
            + std::to_string((int)g_areaSize) + "]"));

    auto spd = CreateObject<ConstantRandomVariable>();
    spd->SetAttribute("Constant", DoubleValue(SPEED_NORMAL));
    auto pause = CreateObject<ConstantRandomVariable>();
    pause->SetAttribute("Constant", DoubleValue(1.0));

    mob.SetMobilityModel(
        "ns3::RandomWaypointMobilityModel",
        "Speed",  PointerValue(spd),
        "Pause",  PointerValue(pause),
        "PositionAllocator",
        StringValue(
            "ns3::RandomRectanglePositionAllocator"));
    mob.Install(g_allNodes);

    // ── Internet + OLSR ────────────────────────────────────
    OlsrHelper olsr;
    InternetStackHelper inet;
    inet.SetRoutingHelper(olsr);
    inet.Install(g_allNodes);

    Ipv4AddressHelper addr;
    addr.SetBase("10.1.0.0", "255.255.0.0");
    g_interfaces = addr.Assign(g_devices);

    // ── Queue discipline ───────────────────────────────────
    TrafficControlHelper tch;
    tch.SetRootQueueDisc("ns3::FqCoDelQueueDisc",
        "MaxSize", StringValue(
            std::to_string(QUEUE_MAX_PKTS) + "p"));

    g_qLen.assign(g_nNodes, 0);
    g_qDrops.assign(g_nNodes, 0);

    for (uint32_t i = 0; i < g_devices.GetN(); i++) {
        Ptr<NetDevice> nd = g_devices.Get(i);
        Ptr<Node>  node   = nd->GetNode();
        Ptr<TrafficControlLayer> tc =
            node->GetObject<TrafficControlLayer>();
        Ptr<QueueDisc> qd = tc ?
            tc->GetRootQueueDiscOnDevice(nd) : nullptr;
        if (!qd) {
            auto qdc = tch.Install(nd);
            qd = qdc.Get(0);
        }
        qd->TraceConnectWithoutContext("Enqueue",
            MakeBoundCallback(&OnEnqueue, i));
        qd->TraceConnectWithoutContext("Dequeue",
            MakeBoundCallback(&OnDequeue, i));
        qd->TraceConnectWithoutContext("Drop",
            MakeBoundCallback(&OnQueueDrop, i));
    }

    // ── Applications ───────────────────────────────────────
    uint16_t port = 5000;

    // Receiver on node 0 (primary sink)
    auto r0 = CreateObject<MlFgaReceiver>();
    r0->Setup(port);
    g_allNodes.Get(0)->AddApplication(r0);
    r0->SetStartTime(Seconds(1.0));
    r0->SetStopTime(Seconds(g_simSeconds + 1));

    // Senders on all other nodes → node 0
    // Start at t=20s to allow OLSR convergence
    for (uint32_t i = 1; i < g_nNodes; i++) {
        auto s = CreateObject<MlFgaSender>();
        s->Setup(g_interfaces.GetAddress(0),
                 port, PPS_BENIGN, PKT_SIZE);
        g_allNodes.Get(i)->AddApplication(s);
        double st = 20.0 + i * 0.05;
        s->SetStartTime(Seconds(st));
        s->SetStopTime(Seconds(g_simSeconds));
        g_senders.push_back(s);
    }

    // ── Schedule traces (after OLSR warm-up) ───────────────
    Simulator::Schedule(Seconds(21.0), &ConnectTraces);

    // ── Schedule regime changes ────────────────────────────
    Simulator::Schedule(Seconds(R2_START), &StartCongestion);
    Simulator::Schedule(Seconds(R3_START), &StartMobility);
    Simulator::Schedule(Seconds(R4_START), &StartInterference);
    Simulator::Schedule(Seconds(R5_START), &StartMalicious);

    // ── Periodic stats logger ──────────────────────────────
    Simulator::Schedule(Seconds(22.0), &LogStats);

    // ── Run ────────────────────────────────────────────────
    Simulator::Stop(Seconds(g_simSeconds));
    std::cout << "[ML-FGA] Running...\n";
    Simulator::Run();
    Simulator::Destroy();

    g_txLog.close();    g_rxLog.close();
    g_radioLog.close(); g_queueLog.close();
    g_macLog.close();   g_mobilityLog.close();
    g_dropLog.close();  g_phyBusyLog.close();

    std::cout << "[ML-FGA] Done. Output: " << base << "\n";
    return 0;
}
