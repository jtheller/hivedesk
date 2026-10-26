#include "hivedesk/config.h"
#include "hivedesk/transport.h"
#include "hivedesk/desktop_aux.h"
#include "hivedesk/adaptive.h"
#include <windows.h>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <stdexcept>
#include <thread>

// Explicit integration probe against this user's running tray agent, on loopback.
// Does not inject input, change the clipboard or persist screen images/keys.
int main() {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    try {
        auto config=hive::load_config();hive::Transport client;std::mutex mutex;std::condition_variable changed;
        uint64_t reply=0;int snapshots=0,frames=0,cursors=0;bool valid_cursor_state=false;std::string failure;
        int automatic_frames=0,automatic_keys=0;hive::StreamHealth health{};hive::SourceDiagnostics diagnostics{};
        hive::TransportCallbacks callbacks;
        callbacks.snapshot=[&](const std::string&,hive::Snapshot snapshot) {
            try {
                auto image=hive::decode_snapshot(snapshot);
                std::lock_guard lock(mutex);reply=snapshot.request;++snapshots;changed.notify_all();
            }catch(const std::exception& e){std::lock_guard lock(mutex);failure=e.what();changed.notify_all();}
        };
        callbacks.frame=[&](const std::string&,hive::EncodedFrame frame){std::lock_guard lock(mutex);if(!frame.data.empty())++frames;if(frame.session==77){++automatic_frames;if(frame.keyframe)++automatic_keys;}changed.notify_all();};
        callbacks.health=[&](const std::string&,const hive::StreamHealth& value){std::lock_guard lock(mutex);health=value;changed.notify_all();};
        callbacks.diagnostics=[&](const std::string&,const hive::SourceDiagnostics& value){std::lock_guard lock(mutex);diagnostics=value;changed.notify_all();};
        callbacks.cursor=[&](const std::string&,hive::CursorState cursor){
            std::lock_guard lock(mutex);++cursors;
            if(cursor.width){HCURSOR handle=hive::make_cursor(cursor);if(handle){valid_cursor_state=true;DestroyCursor(handle);}}
            else if(!cursor.visible)valid_cursor_state=true; // A hidden/off-primary cursor legitimately has no bitmap.
            changed.notify_all();
        };
        std::string error,peer;
        if(!client.start({"aux-probe-"+std::to_string(GetCurrentProcessId()),"Local feature probe",config.group_key,0,false,true},callbacks,error)||
           !client.connect("127.0.0.1",config.port,peer,error))throw std::runtime_error("Probe connection: "+error);
        struct Release {hive::Transport& client;std::string peer;~Release(){client.stop_stream(peer);client.stop();}} release{client,peer};
        const char* phase="snapshots";
        auto wait=[&](auto predicate){std::unique_lock lock(mutex);if(!changed.wait_for(lock,std::chrono::seconds(8),[&]{return !failure.empty()||predicate();})||!failure.empty())throw std::runtime_error(failure.empty()?std::string("Feature reply timed out: ")+phase+", frames="+std::to_string(frames)+", cursors="+std::to_string(cursors)+", shape="+std::to_string(valid_cursor_state)+", auto="+std::to_string(automatic_frames)+", health="+std::to_string(health.rate_millihz):failure);};
        for(uint64_t request:{uint64_t(11),uint64_t(12)}){
            if(!client.request_snapshot(peer,request))throw std::runtime_error("Snapshot request refused");
            wait([&]{return reply==request;});
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        {std::lock_guard lock(mutex);if(snapshots!=2||frames||cursors)throw std::runtime_error("Snapshot started background streaming/cursor polling");}
        for(uint64_t request=13;request<=32;++request)if(!client.request_snapshot(peer,request))throw std::runtime_error("Rapid snapshot request refused");
        wait([&]{return reply==32;});
        {std::lock_guard lock(mutex);if(frames||cursors)throw std::runtime_error("Rapid snapshots started a stream");}
        phase="legacy stream";
        if(!client.request_stream(peer,{60,30}))throw std::runtime_error("Stream request refused");
        wait([&]{return frames>=3&&valid_cursor_state;});
        client.stop_stream(peer);std::this_thread::sleep_for(std::chrono::milliseconds(400));
        int stopped_frames=0,stopped_cursors=0;
        {std::lock_guard lock(mutex);stopped_frames=frames;stopped_cursors=cursors;}
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
        {std::lock_guard lock(mutex);if(frames!=stopped_frames||cursors!=stopped_cursors)throw std::runtime_error("Stream did not stop after release");}
        phase="automatic stream";
        if(!client.request_stream(peer,{0,0,59940,77}))throw std::runtime_error("Automatic request refused");
        wait([&]{return automatic_frames>0&&health.session==77&&health.target_millihz==std::min(hive::primary_refresh_millihz(),59940u);});
        phase="source diagnostics";wait([&]{return diagnostics.session==77&&hive::valid_diagnostics(diagnostics)&&diagnostics.encode_details;});
        phase="automatic pressure";
        if(!client.send_feedback(peer,{77,0,0,100000,0,false}))throw std::runtime_error("Automatic feedback refused");
        wait([&]{return health.rate_millihz<=9000;});
        phase="automatic IDR";
        if(!client.send_feedback(peer,{77,0,0,1000,0,true}))throw std::runtime_error("Recovery feedback refused");
        wait([&]{return automatic_keys>=2;});
        client.stop_stream(peer);
        std::puts("PASS running agent automatic session: min-refresh health, source pacing responds to decoder pressure, explicit recovery IDR, broker-to-wire source diagnostics.");
        std::printf("PASS running agent: requested JPEG snapshots and latest of 20 rapid requests decoded without background video/cursors; %d actual video frames and %d native cursor messages after Start; both stop after release. No input, clipboard writes or saved images.\n",stopped_frames,stopped_cursors);
        return 0;
    }catch(const std::exception& e){std::fprintf(stderr,"FAIL running agent aux probe: %s\n",e.what());return 1;}
}
