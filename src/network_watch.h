#pragma once
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <netioapi.h>
#include <powrprof.h>
#include <atomic>

namespace hive {
// Callbacks only mark work. Cancellation runs on the owner, never in a Windows
// notification callback or while holding transport locks.
class NetworkWatch {
    HANDLE address_=nullptr,interface_=nullptr,route_=nullptr;
    HPOWERNOTIFY power_=nullptr;
    DEVICE_NOTIFY_SUBSCRIBE_PARAMETERS subscription_{};
    std::atomic<bool> changed_{true};
    static void CALLBACK address(void* p,PMIB_UNICASTIPADDRESS_ROW,MIB_NOTIFICATION_TYPE){static_cast<NetworkWatch*>(p)->changed_=true;}
    static void CALLBACK interface_changed(void* p,PMIB_IPINTERFACE_ROW,MIB_NOTIFICATION_TYPE){static_cast<NetworkWatch*>(p)->changed_=true;}
    static void CALLBACK route(void* p,PMIB_IPFORWARD_ROW2,MIB_NOTIFICATION_TYPE){static_cast<NetworkWatch*>(p)->changed_=true;}
    static ULONG CALLBACK power(void* p,ULONG event,void*){
        if(event==PBT_APMRESUMEAUTOMATIC||event==PBT_APMRESUMESUSPEND)static_cast<NetworkWatch*>(p)->changed_=true;return ERROR_SUCCESS;
    }
public:
    void start(){
        NotifyUnicastIpAddressChange(AF_INET,address,this,FALSE,&address_);
        NotifyIpInterfaceChange(AF_INET,interface_changed,this,FALSE,&interface_);
        NotifyRouteChange2(AF_INET,route,this,FALSE,&route_);
        subscription_={power,this};PowerRegisterSuspendResumeNotification(DEVICE_NOTIFY_CALLBACK,&subscription_,&power_);
    }
    ~NetworkWatch(){if(power_)PowerUnregisterSuspendResumeNotification(power_);for(auto h:{address_,interface_,route_})if(h)CancelMibChangeNotify2(h);}
    bool changed(){return changed_.exchange(false);}
};
}
