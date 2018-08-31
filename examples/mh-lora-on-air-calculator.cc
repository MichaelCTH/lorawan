#include "ns3/lora-phy.h"
#include "ns3/log.h"
#include "ns3/command-line.h"

using namespace ns3;

NS_LOG_COMPONENT_DEFINE ("MH-LoRaOnAirCalc");

int main (int argc, char *argv[])
{

    CommandLine cmd;
    cmd.Parse (argc, argv);

    LogComponentEnable ("MH-LoRaOnAirCalc", LOG_LEVEL_ALL);
    //LogComponentEnable ("LoraPhy", LOG_LEVEL_ALL);

    Ptr<Packet> packet;
    Time duration;

    // PayloadSize, SF, HeaderDisabled, CodingRate, Bandwidth, nPreambleSyms, crcEnabled, lowDROptimization
    LoraTxParameters txParams;
    txParams.sf = 7;
    txParams.headerDisabled = false;
    txParams.codingRate = 1;
    txParams.bandwidthHz = 500000;
    txParams.nPreamble = 8;
    txParams.crcEnabled = 1;
    txParams.lowDataRateOptimizationEnabled = 0;

    int MTU = 180;
    int WDS = 80;
    int lenProto = 4;
    int fileSize = 1024*100.0;
    int nACK = fileSize % WDS;
    int nPacket = std::ceil(fileSize/MTU);

    packet = Create<Packet>(MTU+lenProto);

    NS_LOG_DEBUG ("MTU: " << MTU << " bytes, total number of packet: " << nPacket);

    // Total time = (tPreamble + tPayload + tProto) * (nPacket + nACK) + tSPI * (tPayload + tProto)
    duration = LoraPhy::GetOnAirTime(packet, txParams);
    float tTotal  = (nPacket + nACK) * duration.GetSeconds();
    //SPI 5MHz
    tTotal += nPacket * (MTU+lenProto) / 625000.0;
    tTotal *= 1.15;
    NS_LOG_DEBUG ("Computed: " << tTotal << " s, expected: " << " s");

    return 0;
}
