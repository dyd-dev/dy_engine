#include "dyf/Diagnostics/LiveProfiling.h"
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <thread>
using namespace dyf::Diagnostics;
int main(int argc,char** argv)
{
    if(argc!=3||IsLiveCollectorEnabled())return 1;
    const auto pid=std::uint32_t(std::strtoul(argv[1],nullptr,10));const bool desired=argv[2][0]=='1';LiveClient client;LiveId request=0;std::shared_ptr<const LiveSnapshot> held;
    const auto deadline=LiveNowNs()+6000000000ull;
    while(LiveNowNs()<deadline){
        if(client.GetState()==LiveClientState::Disconnected){for(const auto& target:LiveClient::Discover())if(target.processId==pid){client.Connect(target);break;}}
        if(auto snapshot=client.Poll()){held=snapshot;if(!request)for(const auto& control:snapshot->controls)if(control.key=="transport.toggle")request=client.RequestControl(control.id,desired);
            for(const auto& status:snapshot->controlStatus)if(status.request==request&&request){if(status.state==ControlState::Applied&&std::get<bool>(status.actual)==desired){client.Disconnect();if(!held||held->sequence==0)return 2;std::cout<<"applied request="<<request<<" actual="<<desired<<" producer_off_client=1\n";return 0;}if(status.state==ControlState::Rejected||status.state==ControlState::Failed)return 3;}
        }
        if(client.GetState()==LiveClientState::Failed){std::cerr<<client.GetLastError()<<"\n";return 4;}
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    std::cerr<<"No applied control before deadline\n";return 5;
}
