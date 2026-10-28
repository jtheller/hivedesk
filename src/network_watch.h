#pragma once
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <netioapi.h>
#include <atomic>

namespace hive {
// Callbacks only mark work. Cancellation runs on the owner, never in a Windows
// notification callback or while holding transport locks.
class NetworkWatch {
    HANDLE address_=nullptr,interface_=nullptr,route_=nullptr;
    std::atomic<bool> changed_{true};
    static void CALLBACK address(void* p,PMIB_UNICASTIPADDRESS_ROW,MIB_NOTIFICATION_TYPE){static_cast<NetworkWatch*>(p)->changed_=true;}
    static void CALLBACK interface_changed(void* p,PMIB_IPINTERFACE_ROW,MIB_NOTIFICATION_TYPE){static_cast<NetworkWatch*>(p)->changed_=true;}
    static void CALLBACK route(void* p,PMIB_IPFORWARD_ROW2,MIB_NOTIFICATION_TYPE){static_cast<NetworkWatch*>(p)->changed_=true;}
public:
    void start(){
        NotifyUnicastIpAddressChange(AF_INET,address,this,FALSE,&address_);
        NotifyIpInterfaceChange(AF_INET,interface_changed,this,FALSE,&interface_);
        NotifyRouteChange2(AF_INET,route,this,FALSE,&route_);
    }
    ~NetworkWatch(){for(auto h:{address_,interface_,route_})if(h)CancelMibChangeNotify2(h);}
    bool changed(){return changed_.exchange(false);}
};
}
