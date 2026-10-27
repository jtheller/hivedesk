#include "hivedesk/host_bridge.h"
#include "hivedesk/config.h"
#include <windows.h>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace hive {
// Explicit installed-main-app diagnostic. The broker has one pipe instance:
// an existing controller cannot be displaced. No input/hooks/clipboard calls.
int warm_broker_check(){
    std::mutex mutex;std::condition_variable changed;
    unsigned frames=0;uint64_t first=0,second=0;bool fresh_idr=false;
    HostBridge host; // Join callbacks before their state is destroyed.
    try{
        StreamSettings settings;settings.session=GetTickCount64();settings.receiver_millihz=60000;settings.warm=true;
        std::string error;
        if(!host.start(settings,[&](EncodedFrame frame){
            std::lock_guard lock(mutex);
            if(frames==0)first=frame.sequence;
            if(frames==1){second=frame.sequence;fresh_idr=frame.keyframe;}
            ++frames;changed.notify_all();
        },error))throw std::runtime_error(error);
        auto wait=[&](unsigned count,const char* message){
            std::unique_lock lock(mutex);
            if(!changed.wait_for(lock,std::chrono::seconds(6),[&]{return frames>=count;}))throw std::runtime_error(message);
        };
        wait(1,"Warm SYSTEM capture produced no first picture");
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        {std::lock_guard lock(mutex);if(frames!=1)throw std::runtime_error("Warm SYSTEM encoder did not pause");}
        if(!host.activate())throw std::runtime_error("SYSTEM broker activation acknowledgement failed");
        wait(2,"Activated SYSTEM encoder produced no fresh picture");
        host.stop();
        if(!fresh_idr||second<=first)throw std::runtime_error("Promotion lost fresh IDR or frame sequence");
        log("PASS installed warm broker: trusted app, SYSTEM first picture, paused encoder, Activate/Activated roundtrip, resumed fresh IDR and increasing sequence; no input or clipboard.");
        return 0;
    }catch(const std::exception& e){host.stop();log(std::string("FAIL installed warm broker: ")+e.what());return 1;}
}
}
