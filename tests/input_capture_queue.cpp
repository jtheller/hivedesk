#include "../src/input_capture_queue.h"
#include <cstdio>
#include <stdexcept>

namespace {
void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
using hive::CapturedInput;using hive::InputCaptureQueue;using hive::InputEvent;using hive::InputKind;
CapturedInput item(InputKind kind,int32_t a,int32_t b,uint64_t time=100,uint64_t generation=1){return {{kind,a,b,0},generation,time};}
}
int main(){try {
    {
        InputCaptureQueue<> queue;
        // An 8 kHz mouse cannot grow the worker queue while a send is pending.
        for(int i=0;i<8000;++i)require(queue.push(item(InputKind::Absolute,i,i+1,uint64_t(i+100))),"absolute enqueue failed");
        require(queue.size()==1&&queue.coalesced()==7999,"absolute burst did not stay bounded");
        const auto newest=queue.pop();require(newest&&newest->event.a==7999&&newest->event.b==8000&&newest->captured_us==8099,"absolute burst did not retain newest position/time");
    }
    {
        InputCaptureQueue<> queue;
        for(int i=0;i<100;++i)require(queue.push(item(InputKind::Relative,2,-3,uint64_t(100+i))),"relative enqueue failed");
        require(queue.push(item(InputKind::Button,1,0,200)),"button down enqueue failed");
        for(int i=0;i<25;++i)require(queue.push(item(InputKind::Relative,-4,5,uint64_t(201+i))),"drag enqueue failed");
        require(queue.push(item(InputKind::Button,1,1,250)),"button up enqueue failed");
        require(queue.size()==4,"mouse drag crossed a button ordering barrier");
        const auto before=queue.pop(),down=queue.pop(),drag=queue.pop(),up=queue.pop();
        require(before->event.a==200&&before->event.b==-300&&before->captured_us==100,"relative movement lost displacement or oldest age");
        require(down->event.kind==InputKind::Button&&down->event.b==0&&drag->event.a==-100&&drag->event.b==125&&up->event.kind==InputKind::Button&&up->event.b==1,"drag/button release order changed");
    }
    {
        InputCaptureQueue<> queue;
        queue.push(item(InputKind::Relative,INT32_MAX,INT32_MIN));queue.push(item(InputKind::Relative,123,-456));
        const auto bounded=queue.pop();require(bounded->event.a==INT32_MAX&&bounded->event.b==INT32_MIN,"relative accumulation overflowed signed integer");
        queue.push(item(InputKind::Absolute,2,3));queue.push(item(InputKind::Wheel,120,0));queue.push(item(InputKind::Absolute,4,5));
        queue.push(item(InputKind::Key,0x1e,0));queue.push(item(InputKind::Absolute,6,7));queue.push(item(InputKind::Key,0x1e,1));
        require(queue.size()==6,"motion merged across wheel/key boundary");
        const InputKind expected[]={InputKind::Absolute,InputKind::Wheel,InputKind::Absolute,InputKind::Key,InputKind::Absolute,InputKind::Key};
        for(const auto kind:expected)require(queue.pop()->event.kind==kind,"wheel/key ordering changed");
    }
    {
        InputCaptureQueue<4> queue;
        for(int i=0;i<4;++i)require(queue.push(item(InputKind::Key,0x1e,i%2)),"bounded key enqueue failed");
        require(!queue.push(item(InputKind::Key,0x30,1))&&queue.size()==4,"edge overflow was silently accepted");
        // The owner reacts by aborting this session: only an idempotent release
        // remains, so no old held key is replayed after local escape/handoff.
        queue.clear();require(queue.push(item(InputKind::ReleaseAll,0,0)),"emergency release could not be queued");
        require(queue.size()==1&&queue.pop()->event.kind==InputKind::ReleaseAll,"overflow retained stale key transitions");
    }
    {
        InputCaptureQueue<> queue;
        queue.push(item(InputKind::Absolute,1,2,100,1));queue.push(item(InputKind::Absolute,3,4,101,2));
        require(queue.size()==2,"different sessions coalesced");
        queue.clear();queue.push(item(InputKind::Relative,7,8,102,3));
        require(queue.pop()->generation==3&&!queue.pop(),"handoff retained old generation");
        // Repeated wraparound still delivers key down/up and exact motion sums.
        for(int i=0;i<10000;++i){
            queue.push(item(InputKind::Key,0x1e,0));queue.push(item(InputKind::Relative,1,2));queue.push(item(InputKind::Relative,2,3));queue.push(item(InputKind::Key,0x1e,1));
            require(queue.pop()->event.b==0,"wrapped key down lost");const auto motion=queue.pop();
            require(motion->event.a==3&&motion->event.b==5&&queue.pop()->event.b==1&&!queue.pop(),"wrapped stream corrupted motion or key up");
        }
    }
    std::puts("PASS bounded input capture: 8 kHz coalescing, drag/key barriers, exact relative sums, overflow release, session cancellation");return 0;
}catch(const std::exception& e){std::fprintf(stderr,"FAIL input capture queue: %s\n",e.what());return 1;}}
