/*
 * Modify required tx callback to take also packet as input and add counter to see
 * the number of successfully received/interfered/... packets only inside the transients
 */
#include "ns3/point-to-point-module.h"
#include "ns3/forwarder-helper.h"
#include "ns3/network-server-helper.h"
#include "ns3/lora-channel.h"
#include "ns3/mobility-helper.h"
#include "ns3/lora-phy-helper.h"
#include "ns3/lora-mac-helper.h"
#include "ns3/lora-helper.h"
#include "ns3/gateway-lora-phy.h"
#include "ns3/periodic-sender.h"
#include "ns3/periodic-sender-helper.h"
#include "ns3/log.h"
#include "ns3/string.h"
#include "ns3/command-line.h"
#include "ns3/core-module.h"
#include "ns3/network-module.h"
#include "ns3/lora-device-address-generator.h"
#include "ns3/one-shot-sender-helper.h"
#include "ns3/correlated-shadowing-propagation-loss-model.h"
#include "ns3/building-penetration-loss.h"
#include "ns3/building-allocator.h"
#include "ns3/buildings-helper.h"

#include "ns3/end-device-lora-phy.h"
#include "ns3/end-device-lora-mac.h"
#include "ns3/gateway-lora-mac.h"
#include "ns3/simulator.h"
#include "ns3/pointer.h"
#include "ns3/constant-position-mobility-model.h"
#include "ns3/node-container.h"
#include "ns3/position-allocator.h"
#include "ns3/double.h"
#include "ns3/random-variable-stream.h"
#include "ns3/rng-seed-manager.h"
#include <algorithm>
#include <ctime>

#include "ns3/okumura-hata-propagation-loss-model.h"

using namespace ns3;

NS_LOG_COMPONENT_DEFINE ("CompleteNetworkPerformances");

// Network settings
int nDevices = 2000;
int gatewayRings = 1;
int nGateways = 3*gatewayRings*gatewayRings-3*gatewayRings+1;
double radius = 19200;
double gatewayRadius = 7500/((gatewayRings-1)*2+1);
double simulationTime = 600;
int appPeriodSeconds = 600;
int periodsToSimulate = 1;
int transientPeriods = 0;
int run=1;
std::vector<int> sfQuantity (6);

int noMoreReceivers = 0;
int interfered = 0;
int received = 0;
int underSensitivity = 0;
int totalPktsSent = 0;

// RetransmissionParameters
int maxNumbTx = 8;
bool DRAdapt = false;
bool mixedPeriods = false;

// Output control
bool print = false;
bool buildingsEnabled = false;
bool shadowingEnabled = false;

/**********************
 *  Global Callbacks  *
 **********************/

enum PacketOutcome {
  RECEIVED,
  INTERFERED,
  NO_MORE_RECEIVERS,
  UNDER_SENSITIVITY,
  UNSET
};

struct PacketStatus {
  Ptr<Packet const> packet;
  uint32_t senderId;
  int outcomeNumber;
  std::vector<enum PacketOutcome> outcomes;
};

struct RetransmissionStatus {
  Time firstAttempt;
  Time finishTime;
  uint8_t reTxAttempts;
  bool successful;
};

struct MacPacketStatus {
  Time sendTime;
  Time receivedTime;
  uint32_t systemId;
};

typedef std::pair<Time, PacketOutcome> PhyOutcome;
typedef std::map<Ptr<Packet const>, MacPacketStatus> MacPacketData;
typedef std::map<Ptr<Packet const>, PacketStatus> PhyPacketData;
typedef std::map<Ptr<Packet const>, RetransmissionStatus> RetransmissionData;

PhyPacketData packetTracker;

MacPacketData macPacketTracker;

RetransmissionData reTransmissionTracker;

std::list<PhyOutcome> phyPacketOutcomes;

void
CheckReceptionByAllGWsComplete (std::map<Ptr<Packet const>, PacketStatus>::iterator it)
{
  // Check whether this packet is received by all gateways
  // if ((*it).second.outcomeNumber == nGateways)
    {
      // Update the statistics
      PacketStatus status = (*it).second;
      for (int j = 0; j < nGateways; j++)
        {
          switch (status.outcomes.at (j))
            {
            case RECEIVED:
              {
                received += 1;
                break;
              }
            case UNDER_SENSITIVITY:
              {
                underSensitivity += 1;
                break;
              }
            case NO_MORE_RECEIVERS:
              {
                noMoreReceivers += 1;
                break;
              }
            case INTERFERED:
              {
                interfered += 1;
                break;
              }
            case UNSET:
              {
                break;
              }
            }
        }
      // Remove the packet from the tracker
      // packetTracker.erase (it);
    }
}

void
PrintVector (std::vector<int> vector)
{
  // NS_LOG_INFO ("PrintRetransmissions");

  for (int i = 0; i < int(vector.size ()); i++)
    {
      // NS_LOG_INFO ("i: " << i);
      std::cout << vector.at (i) << " ";
    }
  //
    // std::cout << std::endl;
}


void
PrintSumRetransmissions (std::vector<int> reTxVector)
{
  // NS_LOG_INFO ("PrintSumRetransmissions");

  int total = 0;
  for (int i = 0; i < int(reTxVector.size ()); i++)
    {
      // NS_LOG_INFO ("i: " << i);
      total += reTxVector[i] * (i + 1);
    }
  std::cout << total << std::endl;
}

void
CountRetransmissions (Time transient, Time simulationTime, MacPacketData
                      macPacketTracker, RetransmissionData reTransmissionTracker,
                      PhyPacketData packetTracker)
{
  // NS_LOG_INFO ("CountRetransmissions");

  std::vector<int> totalReTxAmounts (8, 0);
  std::vector<int> successfulReTxAmounts (8, 0);
  std::vector<int> failedReTxAmounts (8, 0);
  // vector performanceAmounts will contain - for the interval given in the input of the function,
  // totPacketsSent receivedPackets interferedPackets noMoreGwPackets underSensitivityPackets
  std::vector<int> performancesAmounts (5, 0);
  Time delaySum = Seconds (0);
  Time ackDelaySum = Seconds(0);

  int packetsOutsideTransient = 0;
  int MACpacketsOutsideTransient = 0;

  for (auto itMac = macPacketTracker.begin (); itMac != macPacketTracker.end(); ++itMac)
    {
      // NS_LOG_DEBUG ("Dealing with packet " << (*itMac).first);

      if ((*itMac).second.sendTime >= transient && (*itMac).second.sendTime <= simulationTime - transient)
        {
          packetsOutsideTransient++;
          MACpacketsOutsideTransient++;

          // Count retransmissions
          ////////////////////////
          auto itRetx = reTransmissionTracker.find ((*itMac).first);

          if (itRetx == reTransmissionTracker.end())
            {
              NS_LOG_DEBUG ("Packet " << (*itMac).first << " not found. Sent at " << (*itMac).second.sendTime.GetSeconds());
            }

          // NS_LOG_DEBUG ("Transmission attempts: " << unsigned((*itRetx).second.reTxAttempts));

          totalReTxAmounts.at ((*itRetx).second.reTxAttempts - 1)++;

          if ((*itRetx).second.successful)
            {
              successfulReTxAmounts.at ((*itRetx).second.reTxAttempts - 1)++;
            }
          else
            {
              failedReTxAmounts.at ((*itRetx).second.reTxAttempts - 1)++;
            }

          // Compute delays
          /////////////////
          if ((*itMac).second.receivedTime == Time::Max())
            {
              // NS_LOG_DEBUG ("Packet never received, ignoring it");
              packetsOutsideTransient--;
            }
          else
            {
              delaySum += (*itMac).second.receivedTime - (*itMac).second.sendTime;
              ackDelaySum += (*itRetx).second.finishTime - (*itRetx).second.firstAttempt;
            }

        }
    }

  // Sum PHY outcomes
  //////////////////////////////////

  for (auto itPhy = phyPacketOutcomes.begin(); itPhy != phyPacketOutcomes.end(); ++itPhy)
  {
    if ((*itPhy).first >= transient && (*itPhy).first <= simulationTime - transient)
    {
      performancesAmounts.at(0)++;

      switch ((*itPhy).second)
      {
        case RECEIVED:
          {
            performancesAmounts.at(1)++;
            break;
          }
        case INTERFERED:
          {
            performancesAmounts.at(2)++;
            break;
          }
        case NO_MORE_RECEIVERS:
          {
            performancesAmounts.at(3)++;
            break;
          }
        case UNDER_SENSITIVITY:
          {
            performancesAmounts.at(4)++;
            break;
          }
        case UNSET:
          {
            break;
          }
      }   //end switch
    }
  }
  double avgDelay = (delaySum / packetsOutsideTransient).GetSeconds ();
  double avgAckDelay = ((ackDelaySum) / packetsOutsideTransient).GetSeconds ();

  // this condition because, if not verified, the Retransmission Tracker is empty
  if (transient > Seconds(0))
  {
    // std::cout << "Network performances inside transients: ";
    PrintVector(performancesAmounts);
  }

  // std::cout << "Total number of MAC (app) packets sent in this period: " << MACpacketsOutsideTransient << std::endl;
  // std::cout << "Successful retransmissions: ";
  PrintVector (successfulReTxAmounts);
  // std::cout << "Failed retransmissions: ";
  PrintVector (failedReTxAmounts);

  // std::cout << "Average delay: " << avgDelay << " s" << std::endl;
  std::cout << avgDelay << " ";
  std::cout << avgAckDelay << " ";

  // std::cout << "Total transmitted MAC packets inside the considered period: ";
  PrintSumRetransmissions (totalReTxAmounts);
}

void
MacTransmissionCallback (Ptr<Packet const> packet)
{
  // NS_LOG_INFO ("A new packet arrived at the MAC layer");

  // NS_LOG_INFO ("Packet: " << packet);

  MacPacketStatus status;
  status.sendTime = Simulator::Now ();
  status.receivedTime = Time::Max ();
  status.systemId = Simulator::GetContext ();

  macPacketTracker.insert (std::pair<Ptr<Packet const>, MacPacketStatus> (packet, status));
}

void
TransmissionCallback (Ptr<Packet const> packet, uint32_t systemId)
{
  // NS_LOG_INFO ("Transmitted a packet from device " << systemId);
  // Create a packetStatus
  PacketStatus status;
  status.packet = packet;
  status.senderId = systemId;
  status.outcomeNumber = 0;
  status.outcomes = std::vector<enum PacketOutcome> (nGateways, UNSET);

  packetTracker.insert (std::pair<Ptr<Packet const>, PacketStatus> (packet, status));

  // Update number of transmitted packets
  totalPktsSent= totalPktsSent +1;
}

void
RequiredTransmissionsCallback (uint8_t reqTx, bool success, Time firstAttempt, Ptr<Packet> packet)
{
  // NS_LOG_DEBUG ("ReqTx " << unsigned(reqTx) << ", succ: " << success << ", firstAttempt: " << firstAttempt.GetSeconds ());

  RetransmissionStatus entry;
  entry.firstAttempt = firstAttempt;
  entry.finishTime = Simulator::Now ();
  entry.reTxAttempts = reqTx;
  entry.successful = success;

  reTransmissionTracker.insert (std::pair<Ptr<Packet>, RetransmissionStatus> (packet, entry));
}

void
MacGwReceptionCallback (Ptr<Packet const> packet)
{
  // NS_LOG_INFO ("A packet was successfully received at MAC layer of a gateway");

  // NS_LOG_INFO ("Packet: " << packet);

  // NS_LOG_INFO ("Packet size: " << packet->GetSize());
  // NS_LOG_INFO ("Packet tracker size: " << macPacketTracker.size());

  // Find the received packet in the macPacketTracker
  auto it = macPacketTracker.find(packet);
  if (it != macPacketTracker.end())
    {
      // NS_LOG_INFO ("Found the packet in the tracker");

      (*it).second.receivedTime = Simulator::Now ();

      // NS_LOG_INFO ("Delay for device " << (*it).second.systemId << ": " <<
      //                            ((*it).second.receivedTime -
      //                            (*it).second.sendTime).GetSeconds ());
    }
}

void
PacketReceptionCallback (Ptr<Packet const> packet, uint32_t systemId)
{
  // Remove the successfully received packet from the list of sent ones
  // NS_LOG_INFO ("A packet was successfully received at gateway " << systemId);

  std::map<Ptr<Packet const>, PacketStatus>::iterator it = packetTracker.find (packet);
  (*it).second.outcomes.at (systemId - nDevices) = RECEIVED;
  (*it).second.outcomeNumber += 1;
  // NS_LOG_DEBUG ("Packet received at gateway " << systemId << " at time " << Simulator::Now().GetSeconds()
  //     << ". Outcome at systemId - nDevices= " << systemId - nDevices << " is " << (*it).second.outcomes.at (systemId - nDevices));
  CheckReceptionByAllGWsComplete (it);

  phyPacketOutcomes.push_back (std::pair<Time, PacketOutcome> (Simulator::Now (), RECEIVED));
}

void
InterferenceCallback (Ptr<Packet const> packet, uint32_t systemId)
{
  // NS_LOG_INFO ("A packet was lost because of interference at gateway " << systemId);

  std::map<Ptr<Packet const>, PacketStatus>::iterator it = packetTracker.find (packet);
  (*it).second.outcomes.at (systemId - nDevices) = INTERFERED;
  (*it).second.outcomeNumber += 1;

  CheckReceptionByAllGWsComplete (it);

  phyPacketOutcomes.push_back (std::pair<Time, PacketOutcome> (Simulator::Now (), INTERFERED));
}

void
NoMoreReceiversCallback (Ptr<Packet const> packet, uint32_t systemId)
{
  // NS_LOG_INFO ("A packet was lost because there were no more receivers at gateway " << systemId);

  std::map<Ptr<Packet const>, PacketStatus>::iterator it = packetTracker.find (packet);
  (*it).second.outcomes.at (systemId - nDevices) = NO_MORE_RECEIVERS;
  (*it).second.outcomeNumber += 1;

  CheckReceptionByAllGWsComplete (it);

  phyPacketOutcomes.push_back (std::pair<Time, PacketOutcome> (Simulator::Now (), NO_MORE_RECEIVERS));
}

void
UnderSensitivityCallback (Ptr<Packet const> packet, uint32_t systemId)
{
  // NS_LOG_INFO ("A packet arrived at the gateway under sensitivity at gateway " << systemId);

  std::map<Ptr<Packet const>, PacketStatus>::iterator it = packetTracker.find (packet);
  (*it).second.outcomes.at (systemId - nDevices) = UNDER_SENSITIVITY;
  (*it).second.outcomeNumber += 1;

  CheckReceptionByAllGWsComplete (it);

  phyPacketOutcomes.push_back (std::pair<Time, PacketOutcome> (Simulator::Now (), UNDER_SENSITIVITY));
}

time_t oldtime = std::time (0);

// Periodically print simulation time
void PrintSimulationTime (void)
{
  // NS_LOG_INFO ("Time: " << Simulator::Now().GetHours());
  std::cout << "Simulated time: " << Simulator::Now ().GetHours () << " hours" << std::endl;
  std::cout << "Real time from last call: " << std::time (0) - oldtime << " seconds" << std::endl;
  oldtime = std::time (0);
  Simulator::Schedule (Minutes (30), &PrintSimulationTime);
}

void
PrintEndDevices (NodeContainer endDevices, NodeContainer gateways, std::string filename)
{
  const char * c = filename.c_str ();
  std::ofstream spreadingFactorFile;
  spreadingFactorFile.open (c);
  for (NodeContainer::Iterator j = endDevices.Begin (); j != endDevices.End (); ++j)
    {
      Ptr<Node> object = *j;
      Ptr<MobilityModel> position = object->GetObject<MobilityModel> ();
      NS_ASSERT (position != 0);
      Ptr<NetDevice> netDevice = object->GetDevice (0);
      Ptr<LoraNetDevice> loraNetDevice = netDevice->GetObject<LoraNetDevice> ();
      NS_ASSERT (loraNetDevice != 0);
      Ptr<EndDeviceLoraMac> mac = loraNetDevice->GetMac ()->GetObject<EndDeviceLoraMac> ();
      int sf = int(mac->GetDataRate ());
      Vector pos = position->GetPosition ();
      spreadingFactorFile << pos.x << " " << pos.y << " " << sf << std::endl;
    }
  // Also print the gateways
  for (NodeContainer::Iterator j = gateways.Begin (); j != gateways.End (); ++j)
    {
      Ptr<Node> object = *j;
      Ptr<MobilityModel> position = object->GetObject<MobilityModel> ();
      Vector pos = position->GetPosition ();
      spreadingFactorFile << pos.x << " " << pos.y << " GW" << std::endl;
    }
  spreadingFactorFile.close ();
}

int main (int argc, char *argv[])
{

  CommandLine cmd;
  cmd.AddValue ("nDevices", "Number of end devices to include in the simulation", nDevices);
  cmd.AddValue ("gatewayRings", "Number of gateway rings to include", gatewayRings);
  cmd.AddValue ("radius", "The radius of the area to simulate", radius);
  cmd.AddValue ("gatewayRadius", "The distance between two gateways", gatewayRadius);
  // cmd.AddValue ("simulationTime", "The time for which to simulate", simulationTime);
  cmd.AddValue ("appPeriod", "The period in seconds to be used by periodically transmitting applications", appPeriodSeconds);
  cmd.AddValue ("periodsToSimulate", "The number of application periods to simulate", periodsToSimulate);
  cmd.AddValue ("transientPeriods", "The number of periods we consider as a transient", transientPeriods);
  cmd.AddValue ("maxNumbTx", "The maximum number of transmissions allowed.", maxNumbTx);
  cmd.AddValue ("DRAdapt", "Enable data rate adaptation", DRAdapt);
  cmd.AddValue ("mixedPeriods", "Enable mixed application periods", mixedPeriods);
  cmd.AddValue ("print", "Whether or not to print a file containing the ED's positions and a file containing buildings", print);
  cmd.AddValue("buildingsEnabled", "Whether to use buildings in the simulation or not", buildingsEnabled);
  cmd.AddValue("shadowingEnabled", "Whether to use shadowing in the simulation or not", shadowingEnabled);

  cmd.Parse (argc, argv);

  // Set up logging
  LogComponentEnable ("CompleteNetworkPerformances", LOG_LEVEL_ALL);
  // LogComponentEnable("LoraInterferenceHelper", LOG_LEVEL_ALL);
  // LogComponentEnable("LoraMac", LOG_LEVEL_ALL);
  // LogComponentEnable("LogicalLoraChannel", LOG_LEVEL_ALL);
  // LogComponentEnable("LoraHelper", LOG_LEVEL_ALL);
  // LogComponentEnable("LoraPhyHelper", LOG_LEVEL_ALL);
  // LogComponentEnable("LoraMacHelper", LOG_LEVEL_ALL);
  // LogComponentEnable("LoraFrameHeader", LOG_LEVEL_ALL);
  // LogComponentEnable("LoraMacHeader", LOG_LEVEL_ALL);
  // LogComponentEnable("MacCommand", LOG_LEVEL_ALL);
  // LogComponentEnable("GatewayLoraPhy", LOG_LEVEL_ALL);
  // LogComponentEnable("LoraPhy", LOG_LEVEL_ALL);
  // LogComponentEnable("LoraChannel", LOG_LEVEL_ALL);
  // LogComponentEnable ("EndDeviceLoraPhy", LOG_LEVEL_ALL);
  // LogComponentEnable ("SimpleEndDeviceLoraPhy", LOG_LEVEL_ALL);
  // LogComponentEnable("LogicalLoraChannelHelper", LOG_LEVEL_ALL);
  // LogComponentEnable ("EndDeviceLoraMac", LOG_LEVEL_ALL);
  // LogComponentEnable("PointToPointNetDevice", LOG_LEVEL_ALL);
  // LogComponentEnable("PeriodicSenderHelper", LOG_LEVEL_ALL);
  // LogComponentEnable ("PeriodicSender", LOG_LEVEL_ALL);
  // LogComponentEnable ("SimpleNetworkServer", LOG_LEVEL_ALL);
  // LogComponentEnable ("GatewayLoraMac", LOG_LEVEL_ALL);
  // LogComponentEnable ("Forwarder", LOG_LEVEL_ALL);
  // LogComponentEnable ("DeviceStatus", LOG_LEVEL_ALL);
  // LogComponentEnable ("GatewayStatus", LOG_LEVEL_ALL);
  LogComponentEnableAll (LOG_PREFIX_FUNC);
  LogComponentEnableAll (LOG_PREFIX_NODE);
  LogComponentEnableAll (LOG_PREFIX_TIME);

  /***********
  *  Setup  *
  ***********/

  // Compute the number of gateways
  nGateways = 3*gatewayRings*gatewayRings-3*gatewayRings+1;

  // Create the time value from the period
  Time appPeriod = Seconds (appPeriodSeconds);

  // Mobility
  MobilityHelper mobility;
  // ns3::RngSeedManager::SetRun(run);
  mobility.SetPositionAllocator ("ns3::UniformDiscPositionAllocator",
                                 "rho", DoubleValue (radius),
                                 "X", DoubleValue (0.0),
                                 "Y", DoubleValue (0.0));
  mobility.SetMobilityModel ("ns3::ConstantPositionMobilityModel");

  /************************
  *  Create the channel  *
  ************************/

  // Create the lora channel object
  Ptr<OkumuraHataPropagationLossModel> loss = CreateObject<OkumuraHataPropagationLossModel> ();
  loss->SetAttribute ("Frequency", DoubleValue (868.1e6));
  loss->SetAttribute ("Environment", EnumValue (OpenAreasEnvironment));

  if(shadowingEnabled)
  {
    // Create the correlated shadowing component
    Ptr<CorrelatedShadowingPropagationLossModel> shadowing = CreateObject<CorrelatedShadowingPropagationLossModel> ();

    // Aggregate shadowing to the logdistance loss
    loss->SetNext(shadowing);

    // Add the effect to the channel propagation loss
    Ptr<BuildingPenetrationLoss> buildingLoss = CreateObject<BuildingPenetrationLoss> ();

    shadowing->SetNext(buildingLoss);
  }

  Ptr<PropagationDelayModel> delay = CreateObject<ConstantSpeedPropagationDelayModel> ();

  Ptr<LoraChannel> channel = CreateObject<LoraChannel> (loss, delay);

  /************************
  *  Create the helpers  *
  ************************/

  // Create the LoraPhyHelper
  LoraPhyHelper phyHelper = LoraPhyHelper ();
  phyHelper.SetChannel (channel);

  // Create the LoraMacHelper
  LoraMacHelper macHelper = LoraMacHelper ();

  // Create the LoraHelper
  LoraHelper helper = LoraHelper ();

  /************************
  *  Create End Devices  *
  ************************/

  // Create a set of nodes
  NodeContainer endDevices;
  endDevices.Create (nDevices);

  // Assign a mobility model to each node
  mobility.Install (endDevices);

  // Make it so that nodes are at a certain height > 0
  for (NodeContainer::Iterator j = endDevices.Begin ();
       j != endDevices.End (); ++j)
    {
      Ptr<MobilityModel> mobility = (*j)->GetObject<MobilityModel> ();
      Vector position = mobility->GetPosition ();
      position.z = 1.2;
      mobility->SetPosition (position);
    }

  // Create a LoraDeviceAddressGenerator
  uint8_t nwkId = 54;
  uint32_t nwkAddr = 1864;
  Ptr<LoraDeviceAddressGenerator> addrGen = CreateObject<LoraDeviceAddressGenerator> (nwkId,nwkAddr);

  // Create the LoraNetDevices of the end devices
  phyHelper.SetDeviceType (LoraPhyHelper::ED);
  macHelper.SetDeviceType (LoraMacHelper::ED);
  macHelper.SetAddressGenerator (addrGen);
  helper.Install (phyHelper, macHelper, endDevices);

  // Now end devices are connected to the channel

  // Connect trace sources
  for (NodeContainer::Iterator j = endDevices.Begin ();
       j != endDevices.End (); ++j)
    {
      Ptr<Node> node = *j;
      Ptr<LoraNetDevice> loraNetDevice = node->GetDevice (0)->GetObject<LoraNetDevice> ();
      Ptr<LoraPhy> phy = loraNetDevice->GetPhy ();
      phy->TraceConnectWithoutContext ("StartSending",
                                       MakeCallback (&TransmissionCallback));

      Ptr<EndDeviceLoraMac> mac= loraNetDevice->GetMac ()->GetObject<EndDeviceLoraMac>();

      mac->TraceConnectWithoutContext ("SentNewPacket",
                                       MakeCallback (&MacTransmissionCallback));

      mac->TraceConnectWithoutContext ("RequiredTransmissions",
                                       MakeCallback (&RequiredTransmissionsCallback));
      // Set message type, otherwise the NS does not send ACKs
      mac->SetMType (LoraMacHeader::CONFIRMED_DATA_UP);
      mac-> SetMaxNumberOfTransmissions(maxNumbTx);
      mac->SetDataRateAdaptation(DRAdapt);

    }

  /*********************
  *  Create Gateways  *
  *********************/

  // Create the gateway nodes (allocate them uniformely on the disc)
  NodeContainer gateways;
  gateways.Create (nGateways);

  Ptr<ListPositionAllocator> allocator = CreateObject<ListPositionAllocator> ();
  // Make it so that nodes are at a certain height > 0
  allocator->Add (Vector (0.0, 0.0, 15.0));
  mobility.SetPositionAllocator (allocator);
  mobility.Install (gateways);


  // Create a netdevice for each gateway
  phyHelper.SetDeviceType (LoraPhyHelper::GW);
  macHelper.SetDeviceType (LoraMacHelper::GW);
  helper.Install (phyHelper, macHelper, gateways);

  /************************
  *  Configure Gateways  *
  ************************/

  // Install reception paths on gateways
  for (NodeContainer::Iterator j = gateways.Begin (); j != gateways.End (); j++)
    {

      Ptr<Node> object = *j;
      // Get the device
      Ptr<NetDevice> netDevice = object->GetDevice (0);
      Ptr<LoraNetDevice> loraNetDevice = netDevice->GetObject<LoraNetDevice> ();
      NS_ASSERT (loraNetDevice != 0);
      Ptr<GatewayLoraPhy> gwPhy = loraNetDevice->GetPhy ()->GetObject<GatewayLoraPhy> ();


      // Global callbacks (every gateway)
      gwPhy->TraceConnectWithoutContext ("ReceivedPacket",
                                         MakeCallback (&PacketReceptionCallback));
      gwPhy->TraceConnectWithoutContext ("LostPacketBecauseInterference",
                                         MakeCallback (&InterferenceCallback));
      gwPhy->TraceConnectWithoutContext ("LostPacketBecauseNoMoreReceivers",
                                         MakeCallback (&NoMoreReceiversCallback));
      gwPhy->TraceConnectWithoutContext ("LostPacketBecauseUnderSensitivity",
                                         MakeCallback (&UnderSensitivityCallback));

      Ptr<GatewayLoraMac> gwMac = loraNetDevice->GetMac ()->GetObject<GatewayLoraMac> ();
      gwMac->TraceConnectWithoutContext ("ReceivedPacket",
                                         MakeCallback (&MacGwReceptionCallback));
    }

  /**********************
  *  Handle buildings  *
  **********************/

  double xLength = 130;
  double deltaX = 32;
  double yLength = 64;
  double deltaY = 17;
  int gridWidth = 2*radius/(xLength+deltaX);
  int gridHeight = 2*radius/(yLength+deltaY);
  if (buildingsEnabled == false)
  {
    gridWidth = 0;
    gridHeight = 0;
  }
  Ptr<GridBuildingAllocator> gridBuildingAllocator;
  gridBuildingAllocator = CreateObject<GridBuildingAllocator> ();
  gridBuildingAllocator->SetAttribute ("GridWidth", UintegerValue (gridWidth));
  gridBuildingAllocator->SetAttribute ("LengthX", DoubleValue (xLength));
  gridBuildingAllocator->SetAttribute ("LengthY", DoubleValue (yLength));
  gridBuildingAllocator->SetAttribute ("DeltaX", DoubleValue (deltaX));
  gridBuildingAllocator->SetAttribute ("DeltaY", DoubleValue (deltaY));
  gridBuildingAllocator->SetAttribute ("Height", DoubleValue (6));
  gridBuildingAllocator->SetBuildingAttribute ("NRoomsX", UintegerValue (2));
  gridBuildingAllocator->SetBuildingAttribute ("NRoomsY", UintegerValue (4));
  gridBuildingAllocator->SetBuildingAttribute ("NFloors", UintegerValue (2));
  gridBuildingAllocator->SetAttribute ("MinX", DoubleValue (-gridWidth*(xLength+deltaX)/2+deltaX/2));
  gridBuildingAllocator->SetAttribute ("MinY", DoubleValue (-gridHeight*(yLength+deltaY)/2+deltaY/2));
  BuildingContainer bContainer = gridBuildingAllocator->Create (gridWidth * gridHeight);

  BuildingsHelper::Install (endDevices);
  BuildingsHelper::Install (gateways);
  BuildingsHelper::MakeMobilityModelConsistent ();

  // Print the buildings
  if (print)
  {
    std::ofstream myfile;
    myfile.open ("buildings.txt");
    std::vector<Ptr<Building> >::const_iterator it;
    int j = 1;
    for (it = bContainer.Begin (); it != bContainer.End (); ++it, ++j)
    {
      Box boundaries = (*it)->GetBoundaries ();
      myfile << "set object " << j << " rect from " << boundaries.xMin << "," << boundaries.yMin << " to " << boundaries.xMax << "," << boundaries.yMax << std::endl;
    }
    myfile.close();

  }

  /**********************************************
  *  Set up the end device's spreading factor  *
  **********************************************/

  sfQuantity = macHelper.SetSpreadingFactorsUp (endDevices, gateways, channel);

  /**********************************************
  *              Create Network Server          *
  **********************************************/

  NodeContainer networkServers;
  networkServers.Create (1);

  // Install the SimpleNetworkServer application on the network server
  NetworkServerHelper networkServerHelper;
  networkServerHelper.SetGateways (gateways);
  networkServerHelper.SetEndDevices (endDevices);
  networkServerHelper.Install (networkServers);

  // Install the Forwarder application on the gateways
  ForwarderHelper forwarderHelper;
  forwarderHelper.Install (gateways);


  // NS_LOG_DEBUG ("Completed configuration");

  /*********************************************
  *  Install applications on the end devices  *
  *********************************************/

  PeriodicSenderHelper appHelper = PeriodicSenderHelper ();

  if (mixedPeriods)
  {
    appHelper.SetPeriod (Seconds(0));
    // In this case, as application period we take
    // the maximum of the possible application periods, i.e. 1 day
    appPeriodSeconds= (24*60*60);
    appPeriod = Seconds(appPeriodSeconds);
  }
  else
  {
    appHelper.SetPeriod (appPeriod);
  }

  ApplicationContainer appContainer = appHelper.Install (endDevices);

  Time appStopTime = appPeriod * periodsToSimulate;

  appContainer.Start (Seconds (0));
  appContainer.Stop (appStopTime);

  /**********************
   * Print output files *
   *********************/
  if (print)
    {
      PrintEndDevices (endDevices, gateways,
                       "src/lorawan/examples/endDevices.dat");
    }

  /****************
  *  Simulation  *
  ****************/

  Simulator::Stop (appStopTime + Hours (1000));                    // Stop later to permit the retransmission procedure

  // PrintSimulationTime ();

  Simulator::Run ();

  Simulator::Destroy ();

  /*************
  *  Results  *
  *************/

  // Total statistics of the network 
  std::cout << nDevices << " " << appPeriodSeconds << " ";

  if (transientPeriods == 0)
  {
    std::cout<< totalPktsSent 
      << " " << received << " " << interfered << " " << noMoreReceivers 
      << " " << underSensitivity <<" ";
  }

  // Statistics ignoring transient
  CountRetransmissions (transientPeriods * appPeriod, appStopTime, macPacketTracker, reTransmissionTracker, packetTracker);

  return 0;
}
