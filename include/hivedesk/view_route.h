#pragma once
#include <algorithm>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace hive {
// Reverse viewing chain: [this host, its viewer, that viewer's viewer, ...].
// Only promoted control or visible Peek contributes an edge, never warmup.
struct ViewRoute {
    uint64_t revision=0,session=0;
    std::vector<std::string> path;
    bool full=false;
    bool operator==(const ViewRoute&)const=default;
    std::string cycle_owner()const {
        for(size_t i=0;i<path.size();++i){
            const auto prior=std::find(path.begin(),path.begin()+i,path[i]);
            if(prior!=path.begin()+i)return *std::max_element(prior,path.begin()+i);
        }
        return {};
    }
};
class ViewRoutes {
public:
    static constexpr size_t limit=64;
    std::string self,parent;uint64_t session=0;
    ViewRoute local;
    std::map<std::string,ViewRoute> peers;
    bool rebuild(){
        ViewRoute next;next.session=session;next.path={self};
        if(!parent.empty()){
            std::vector<std::string> upstream{parent};
            if(const auto it=peers.find(parent);it!=peers.end()) {upstream=it->second.path;next.full=it->second.full;}
            for(const auto& node:upstream){
                if(next.path.size()==limit){next.full=true;break;}
                const bool repeated=std::find(next.path.begin(),next.path.end(),node)!=next.path.end();
                next.path.push_back(node);if(repeated)break;
            }
        }
        next.revision=local.revision;
        if(next==local)return false;
        next.revision++;local=std::move(next);return true;
    }
    bool allowed(const std::string& peer)const {
        return peer!=self&&!local.full&&std::find(local.path.begin(),local.path.end(),peer)==local.path.end();
    }
    bool incoming_allowed(const std::string& peer)const {
        const auto it=peers.find(peer);if(it==peers.end())return peer!=self;
        return !it->second.full&&std::find(it->second.path.begin(),it->second.path.end(),self)==it->second.path.end();
    }
};
}
