#include "hivedesk/host_bridge.h"
#include "hivedesk/config.h"
#include <windows.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace hive {
// Explicit installed-main-app diagnostic. The broker has one pipe instance:
// an existing controller cannot be displaced. No input/hooks/clipboard calls.
int warm_broker_check(bool peek){
    std::mutex mutex;std::condition_variable changed;
    unsigned frames=0,restarted=0,activation_baseline=UINT_MAX;uint64_t first=0,second=0;bool fresh_idr=false;
    std::atomic<bool> downstream_ready{true};std::atomic<unsigned> reports{0};
    HostBridge host; // Join callbacks before their state is destroyed.
    try{
        StreamSettings settings;settings.session=GetTickCount64();settings.receiver_millihz=60000;settings.warm=true;
        std::string error;
        if(!host.start(settings,[&](EncodedFrame frame){
            std::lock_guard lock(mutex);
            if(frames==0)first=frame.sequence;
            if(frames==activation_baseline){second=frame.sequence;fresh_idr=frame.keyframe;}
            ++frames;changed.notify_all();
        },error,[&]{return downstream_ready.load();},{},[&](const SourceDiagnostics&){++reports;}))throw std::runtime_error(error);
        auto wait=[&](unsigned count,const char* message){
            std::unique_lock lock(mutex);
            if(!changed.wait_for(lock,std::chrono::seconds(6),[&]{return frames>=count;}))throw std::runtime_error(message);
        };
        wait(1,"Warm SYSTEM capture produced no first picture");
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        {std::lock_guard lock(mutex);if(frames!=1)throw std::runtime_error("Warm SYSTEM encoder did not pause");}
        if(peek){
            if(!host.preview(true))throw std::runtime_error("Read-only preview request failed");
            wait(4,"Read-only preview did not produce continuous pictures");
            if(!host.preview(false))throw std::runtime_error("Preview pause failed");
            std::this_thread::sleep_for(std::chrono::milliseconds(300));unsigned paused;{std::lock_guard lock(mutex);paused=frames;}
            std::this_thread::sleep_for(std::chrono::milliseconds(300));{std::lock_guard lock(mutex);if(frames!=paused)throw std::runtime_error("Preview pause kept encoding");}
            if(!host.preview(true))throw std::runtime_error("Preview resume failed");wait(paused+3,"Preview resume stalled");
            if(!host.preview(false))throw std::runtime_error("Final preview pause failed");
            std::this_thread::sleep_for(std::chrono::milliseconds(350));
            log("PASS installed read-only preview: continuous pictures, pause and resume; no input or clipboard calls.");
        }
        // Reproduce a slow network holding frame credit. The pipe reader must
        // still drain Activated and current status while media is backpressured.
        downstream_ready=false;unsigned before_block;
        {std::lock_guard lock(mutex);before_block=frames;}
        host.request_keyframe();wait(before_block+1,"Blocked-credit setup produced no picture");
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        {std::lock_guard lock(mutex);activation_baseline=frames;}
        if(!host.activate())throw std::runtime_error("SYSTEM broker activation acknowledgement failed");
        const auto before_reports=reports.load();
        std::this_thread::sleep_for(std::chrono::milliseconds(350));
        {std::lock_guard lock(mutex);if(frames!=activation_baseline)throw std::runtime_error("Broker granted video credit while downstream blocked");}
        if(reports.load()<=before_reports)throw std::runtime_error("Blocked video credit stopped source diagnostics");
        downstream_ready=true;host.notify_ready();
        wait(activation_baseline+1,"Activated SYSTEM encoder produced no fresh picture");
        downstream_ready=false;
        {std::lock_guard lock(mutex);before_block=frames;}
        host.request_keyframe();wait(before_block+1,"Stop setup produced no picture");
        const auto stop_at=GetTickCount64();
        host.stop();
        if(GetTickCount64()-stop_at>=1500)throw std::runtime_error("Stop waited on blocked network credit");
        if(!fresh_idr||second<=first)throw std::runtime_error("Promotion lost fresh IDR or frame sequence");
        // Reuse the same bridge; a cancelled pending credit must not leak into
        // the next warm session and accidentally turn it into continuous video.
        downstream_ready=true;settings.session++;
        if(!host.start(settings,[&](EncodedFrame){std::lock_guard lock(mutex);++restarted;changed.notify_all();},error,[&]{return downstream_ready.load();}))throw std::runtime_error(error);
        {std::unique_lock lock(mutex);if(!changed.wait_for(lock,std::chrono::seconds(6),[&]{return restarted!=0;}))throw std::runtime_error("Bridge reuse produced no warm picture");}
        std::this_thread::sleep_for(std::chrono::milliseconds(300));host.stop();
        if(restarted!=1)throw std::runtime_error("Cancelled credit leaked across sessions");
        log("PASS installed warm broker: blocked-credit Activate/Activated, continuing diagnostics, no premature credit, fresh IDR after wake, cancelled credit stop and bridge reuse; no input or clipboard.");
        return 0;
    }catch(const std::exception& e){host.stop();log(std::string("FAIL installed warm broker: ")+e.what());return 1;}
}
}
