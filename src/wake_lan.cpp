#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <netioapi.h>
#include "wake_lan.h"

namespace hive::wake { namespace {
struct Adapter {uint32_t address,mask;Mac mac;};
std::vector<Adapter> adapters(){
    ULONG size=16384;std::vector<uint8_t> bytes(size);ULONG result=ERROR_BUFFER_OVERFLOW;
    for(unsigned n=0;n<3&&result==ERROR_BUFFER_OVERFLOW;++n){
        bytes.resize(size);result=GetAdaptersAddresses(AF_INET,GAA_FLAG_SKIP_ANYCAST|GAA_FLAG_SKIP_MULTICAST|GAA_FLAG_SKIP_DNS_SERVER,nullptr,reinterpret_cast<IP_ADAPTER_ADDRESSES*>(bytes.data()),&size);
    }
    std::vector<Adapter> list;if(result!=NO_ERROR)return list;
    for(auto* a=reinterpret_cast<IP_ADAPTER_ADDRESSES*>(bytes.data());a;a=a->Next){
        if(a->OperStatus!=IfOperStatusUp||(a->IfType!=IF_TYPE_ETHERNET_CSMACD&&a->IfType!=IF_TYPE_IEEE80211)||a->PhysicalAddressLength!=6)continue;
        Mac mac{};std::copy_n(a->PhysicalAddress,6,mac.begin());if(!valid(mac))continue;
        for(auto* u=a->FirstUnicastAddress;u;u=u->Next){
            if(!u->Address.lpSockaddr||u->Address.lpSockaddr->sa_family!=AF_INET||u->OnLinkPrefixLength==0||u->OnLinkPrefixLength>30)continue;
            list.push_back({ntohl(reinterpret_cast<sockaddr_in*>(u->Address.lpSockaddr)->sin_addr.s_addr),0xffffffffu<<(32-u->OnLinkPrefixLength),mac});
        }
    }return list;
}
bool parse(const std::string& address,IN_ADDR& ip){return inet_pton(AF_INET,address.c_str(),&ip)==1&&(ntohl(ip.s_addr)>>24)!=127;}
}
Mac local_mac(const std::string& address){IN_ADDR ip{};if(parse(address,ip))for(const auto& a:adapters())if(a.address==ntohl(ip.s_addr))return a.mac;return {};}
Mac neighbor_mac(const std::string& address){
    IN_ADDR ip{};if(!parse(address,ip))return {};
    // Never learn a routed target's gateway MAC as the target's identity.
    bool local=false;for(const auto& a:adapters())if((a.address&a.mask)==(ntohl(ip.s_addr)&a.mask))local=true;
    if(!local)return {};PMIB_IPNET_TABLE2 table=nullptr;if(GetIpNetTable2(AF_INET,&table)!=NO_ERROR)return {};
    Mac mac{};for(ULONG i=0;i<table->NumEntries;++i){const auto& row=table->Table[i];
        if(row.Address.Ipv4.sin_addr.s_addr==ip.s_addr&&row.PhysicalAddressLength==6&&row.State!=NlnsUnreachable&&row.State!=NlnsIncomplete){std::copy_n(row.PhysicalAddress,6,mac.begin());break;}}
    FreeMibTable(table);return valid(mac)?mac:Mac{};
}
unsigned send(const std::string& address,const Mac& mac){
    IN_ADDR ip{};if(!valid(mac)||!parse(address,ip))return 0;const auto packet=magic(mac);unsigned sent=0;
    for(const auto& a:adapters()){
        const uint32_t target=ntohl(ip.s_addr);if((target&a.mask)!=(a.address&a.mask)||target==a.address)continue;
        const SOCKET s=socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);if(s==INVALID_SOCKET)continue;
        BOOL broadcast=TRUE;setsockopt(s,SOL_SOCKET,SO_BROADCAST,reinterpret_cast<const char*>(&broadcast),sizeof(broadcast));
        int ttl=1;setsockopt(s,IPPROTO_IP,IP_TTL,reinterpret_cast<const char*>(&ttl),sizeof(ttl));u_long nonblocking=1;ioctlsocket(s,FIONBIO,&nonblocking);
        sockaddr_in from{};from.sin_family=AF_INET;from.sin_addr.s_addr=htonl(a.address);
        if(bind(s,reinterpret_cast<sockaddr*>(&from),sizeof(from))==0)for(auto destination:{target,(a.address&a.mask)|~a.mask}){
            sockaddr_in to{};to.sin_family=AF_INET;to.sin_port=htons(9);to.sin_addr.s_addr=htonl(destination);
            if(sendto(s,reinterpret_cast<const char*>(packet.data()),int(packet.size()),0,reinterpret_cast<sockaddr*>(&to),sizeof(to))==packet.size())++sent;
        }closesocket(s);
    }return sent;
}
}
