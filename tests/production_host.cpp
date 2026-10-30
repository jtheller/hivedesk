#include "hivedesk/media.h"
#include "../src/software_decoder.h"
#include "../src/hevc_format.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>
#include <stdexcept>
// Explicit real-primary capture qualification. No input, clipboard, network,
// saved pictures or display-mode changes; never part of automatic CTest.
void intel_pipeline_check();
int main(){
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);setvbuf(stdout,nullptr,_IONBF,0);
    try{
        intel_pipeline_check();
        hive::MediaHost host;hive::SoftwareDecoder decoder;std::atomic<unsigned> frames=0;std::atomic<bool> recovered=false;
        hive::StreamSettings settings;settings.session=1;settings.receiver_millihz=60000;
        std::string error;
        if(!host.start(settings,[&](hive::EncodedFrame frame){
            const auto n=frames.load();
            if(frame.keyframe){auto sps=hive::hevc_annexb_format(frame.data);if(!sps||!sps->full_chroma8()||sps->width!=frame.width||sps->height!=frame.height)throw std::runtime_error("Production host changed chroma or dimensions");if(n)recovered=true;}
            decoder.decode(frame,[&](const hive::SoftwarePicture& picture){if(n<4)std::printf("desktop frame=%u sequence=%llu size=%ux%u key=%d decode_us=%u bytes=%zu\n",n,frame.sequence,picture.width,picture.height,frame.keyframe,picture.decode_us,frame.data.size());});++frames;
        },error)){std::printf("FAIL host initialization: %s\n",error.c_str());return 1;}
        const auto until=GetTickCount64()+6000;bool refresh=false;
        while(host.running()&&GetTickCount64()<until){if(frames.load()&&!refresh){host.request_keyframe();refresh=true;}if(recovered)break;Sleep(10);}
        host.stop();const auto failure=host.failure();if(!failure.empty())throw std::runtime_error(failure);
        if(!recovered)throw std::runtime_error("Production desktop did not emit and decode an initial frame plus requested recovery IDR");
        std::printf("PASS production primary capture, full-range GPU color path, HEVC444 encode/software decode, recovery IDR and stop; frames=%u (not throughput validation)\n",frames.load());return 0;
    }catch(const std::exception& error){std::printf("FAIL %s\n",error.what());return 1;}
}
