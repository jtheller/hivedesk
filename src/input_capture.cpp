#include "input_capture.h"
#include "input_capture_queue.h"
#include "hivedesk/fixed_hotkeys.h"
#include "hivedesk/input_channel.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <stdexcept>
#include <vector>

namespace hive {
struct InputCapture::Impl {
    HINSTANCE instance{};HWND owner{},window{};UINT action_message{};
    HHOOK keyboard{},mouse{};DWORD capture_thread_id{};
    Send send;
    std::thread capture_thread,sender_thread;
    mutable std::mutex mutex;
    std::condition_variable wake,started;
    bool initialized=false,start_ok=false,stopping=false,raw_available=false;
    std::string start_error;
    std::shared_ptr<const InputCaptureState> state=std::make_shared<InputCaptureState>();
    uint64_t route=1,blocked_route=0;
    InputCaptureQueue<> queue;
    PhysicalPointer pointer;
    InputCaptureDiagnostics stats;
    struct Delay {uint64_t at=0;uint32_t us=0;};
    std::array<Delay,256> delays{};size_t delay_index=0;
    // These are exclusively owned by the capture message thread.
    FixedHotkeys fixed;
    std::array<bool,6> buttons{};
    DWORD summon_key_up=0;
    uint64_t observed_route=0;
    static thread_local Impl* hooks;

    struct Route {std::shared_ptr<const InputCaptureState> state;uint64_t id;bool enabled;};
    Route snapshot() {
        std::lock_guard lock(mutex);
        return {state,route,state->remote&&blocked_route!=route&&!stopping&&(!state->windowed||GetForegroundWindow()==state->viewer)};
    }
    void observe_route(uint64_t id) {
        if(observed_route==id)return;
        observed_route=id;fixed.clear_forwarded();buttons.fill(false);
    }
    void post(CaptureAction action,uint64_t generation) {
        if(owner)PostMessageW(owner,action_message,WPARAM(action),LPARAM(generation));
    }
    void fail_route(const Route& target,bool expired) {
        {
            std::lock_guard lock(mutex);
            if(route!=target.id||blocked_route==route)return;
            blocked_route=route;queue.clear();
            queue.push({{InputKind::ReleaseAll},route,input_now_us()});
            if(expired)++stats.expired;else ++stats.overflows;
        }
        wake.notify_one();post(CaptureAction::Failed,target.state->generation);
    }
    void emit(const Route& target,InputEvent event,uint64_t captured_us) {
        if(event.kind==InputKind::Key)fixed.forwarded(uint32_t(event.a),event.c!=0,event.b!=0);
        else if(event.kind==InputKind::ReleaseAll){fixed.clear_forwarded();buttons.fill(false);}
        bool overflow=false;
        {
            std::lock_guard lock(mutex);
            if(stopping||target.id!=route||(blocked_route==route&&event.kind!=InputKind::ReleaseAll))return;
            ++stats.captured;overflow=!queue.push({event,route,captured_us});
        }
        if(overflow)fail_route(target,false);else wake.notify_one();
    }
    void escape(const Route& target,uint64_t now) {
        // Cancel queued input and stop interception before asking the UI to
        // snapshot/hide its viewer. The hook never waits for DWM or the network.
        {
            std::lock_guard lock(mutex);
            if(route==target.id){blocked_route=route;queue.clear();if(target.enabled)queue.push({{InputKind::ReleaseAll},route,now});}
        }
        fixed.clear_forwarded();buttons.fill(false);wake.notify_one();
        post(CaptureAction::ReturnLocal,target.state->generation);
    }
    static bool own_surface(const InputCaptureState& s,POINT p) {
        return s.panel_dragging||(s.panel_visible&&PtInRect(&s.panel_rect,p))||(s.settings_visible&&PtInRect(&s.settings_rect,p));
    }
    static bool map_absolute(const InputCaptureState& s,POINT point,InputEvent& event) {
        double width=s.windowed?s.viewer_rect.right-s.viewer_rect.left:s.screen_width,height=s.windowed?s.viewer_rect.bottom-s.viewer_rect.top:s.screen_height;
        if(s.windowed){point.x-=s.viewer_rect.left;point.y-=s.viewer_rect.top;}
        double content_width=width,content_height=height;
        if(s.remote_width&&s.remote_height){const double scale=std::min(width/s.remote_width,height/s.remote_height);content_width=s.remote_width*scale;content_height=s.remote_height*scale;}
        const double left=(width-content_width)*.5,top=(height-content_height)*.5;
        const bool inside=point.x>=left&&point.x<left+content_width&&point.y>=top&&point.y<top+content_height;
        event={InputKind::Absolute,int32_t(std::clamp((point.x+.5-left)/std::max(1.0,content_width),0.0,1.0)*65535),
            int32_t(std::clamp((point.y+.5-top)/std::max(1.0,content_height),0.0,1.0)*65535)};
        return inside;
    }
    static LRESULT CALLBACK keyboard_proc(int code,WPARAM msg,LPARAM data) {
        auto* s=hooks;if(code<0||!s)return CallNextHookEx(nullptr,code,msg,data);
        const auto& k=*reinterpret_cast<KBDLLHOOKSTRUCT*>(data);
        if(k.flags&LLKHF_INJECTED)return CallNextHookEx(nullptr,code,msg,data);
        const auto now=input_now_us();const auto route=s->snapshot();s->observe_route(route.id);
        const bool up=(k.flags&LLKHF_UP)!=0;
        const auto fixed=s->fixed.observe(k.vkCode,k.scanCode,(k.flags&LLKHF_EXTENDED)!=0,up,false);
        if(fixed.consume) {
            if(fixed.action==FixedHotkey::KickController) {
                if(route.enabled)s->fixed.release_forwarded_modifiers([&](uint32_t scan,bool extended){s->emit(route,{InputKind::Key,int32_t(scan),1,extended?1:0},now);});
                s->post(CaptureAction::KickController,route.state->generation);
            }else if(fixed.action==FixedHotkey::ReturnLocal)s->escape(route,now);
            return 1;
        }
        if(s->summon_key_up==k.vkCode){if(up)s->summon_key_up=0;return 1;}
        if(!up&&k.vkCode==VK_ESCAPE&&route.state->panel_visible){s->summon_key_up=k.vkCode;s->post(CaptureAction::HidePanel,route.state->generation);return 1;}
        if(route.enabled&&!route.state->recording_hotkey&&!up&&k.vkCode==route.state->hotkey_key&&s->fixed.modifiers()==route.state->hotkey_modifiers){
            s->summon_key_up=k.vkCode;s->emit(route,{InputKind::ReleaseAll},now);s->post(CaptureAction::Summon,route.state->generation);return 1;
        }
        if(route.enabled&&!route.state->settings_visible){
            if(k.scanCode>0&&k.scanCode<=255)s->emit(route,{InputKind::Key,int32_t(k.scanCode),up?1:0,(k.flags&LLKHF_EXTENDED)?1:0},now);
            return 1;
        }
        return CallNextHookEx(nullptr,code,msg,data);
    }
    static LRESULT CALLBACK mouse_proc(int code,WPARAM msg,LPARAM data) {
        auto* s=hooks;if(code<0||!s)return CallNextHookEx(nullptr,code,msg,data);
        const auto& m=*reinterpret_cast<MSLLHOOKSTRUCT*>(data);
        if(m.flags&LLMHF_INJECTED)return CallNextHookEx(nullptr,code,msg,data);
        const auto now=input_now_us();const auto route=s->snapshot();s->observe_route(route.id);
        if(route.state->panel_summoned&&(msg==WM_LBUTTONDOWN||msg==WM_RBUTTONDOWN)&&!own_surface(*route.state,m.pt))s->post(CaptureAction::HidePanelAnimated,route.state->generation);
        if(msg==WM_MOUSEMOVE){std::lock_guard lock(s->mutex);++s->pointer.revision;s->pointer.point=m.pt;s->pointer.motion_tick=GetTickCount64();}
        if(!route.enabled)return CallNextHookEx(nullptr,code,msg,data);
        const int released=msg==WM_LBUTTONUP?1:msg==WM_RBUTTONUP?2:msg==WM_MBUTTONUP?3:msg==WM_XBUTTONUP?(HIWORD(m.mouseData)==XBUTTON1?4:5):0;
        if(own_surface(*route.state,m.pt)||(route.state->windowed&&(!PtInRect(&route.state->viewer_rect,m.pt)||GetAncestor(WindowFromPoint(m.pt),GA_ROOTOWNER)!=route.state->viewer))) {
            if(released&&s->buttons[size_t(released)]){s->buttons[size_t(released)]=false;s->emit(route,{InputKind::Button,released,1},now);return 1;}
            return CallNextHookEx(nullptr,code,msg,data);
        }
        if(msg==WM_MOUSEMOVE) {
            if(route.state->relative)return CallNextHookEx(nullptr,code,msg,data);
            InputEvent position{};const bool inside=map_absolute(*route.state,m.pt,position);
            if(inside||std::any_of(s->buttons.begin(),s->buttons.end(),[](bool held){return held;}))s->emit(route,position,now);
            return CallNextHookEx(nullptr,code,msg,data);
        }
        if(!route.state->relative){
            InputEvent position{};const bool inside=map_absolute(*route.state,m.pt,position);
            if(!inside&&!(released&&s->buttons[size_t(released)]))return route.state->windowed?CallNextHookEx(nullptr,code,msg,data):1;
            s->emit(route,position,now);
        }
        InputEvent event{};
        switch(msg){
        case WM_LBUTTONDOWN:event={InputKind::Button,1,0};break;case WM_LBUTTONUP:event={InputKind::Button,1,1};break;
        case WM_RBUTTONDOWN:event={InputKind::Button,2,0};break;case WM_RBUTTONUP:event={InputKind::Button,2,1};break;
        case WM_MBUTTONDOWN:event={InputKind::Button,3,0};break;case WM_MBUTTONUP:event={InputKind::Button,3,1};break;
        case WM_XBUTTONDOWN:case WM_XBUTTONUP:event={InputKind::Button,HIWORD(m.mouseData)==XBUTTON1?4:5,msg==WM_XBUTTONUP?1:0};break;
        case WM_MOUSEWHEEL:case WM_MOUSEHWHEEL:event={InputKind::Wheel,static_cast<short>(HIWORD(m.mouseData)),msg==WM_MOUSEHWHEEL?1:0};break;
        default:return CallNextHookEx(nullptr,code,msg,data);
        }
        if(event.kind==InputKind::Button)s->buttons[size_t(event.a)]=event.b==0;
        s->emit(route,event,now);return 1;
    }
    static LRESULT CALLBACK window_proc(HWND window,UINT message,WPARAM w,LPARAM l) {
        auto* s=hooks;
        if(message==WM_INPUT&&s){
            const auto route=s->snapshot();s->observe_route(route.id);
            RAWINPUT input{};UINT bytes=sizeof(input);
            if(route.enabled&&route.state->relative&&!route.state->settings_visible&&!route.state->panel_visible&&!route.state->panel_dragging&&GetRawInputData(reinterpret_cast<HRAWINPUT>(l),RID_INPUT,&input,&bytes,sizeof(RAWINPUTHEADER))!=UINT(-1)
                &&input.header.dwType==RIM_TYPEMOUSE&&input.header.hDevice&&!(input.data.mouse.usFlags&MOUSE_MOVE_ABSOLUTE)){
                const LONG dx=input.data.mouse.lLastX,dy=input.data.mouse.lLastY;
                if(dx||dy)s->emit(route,{InputKind::Relative,dx,dy},input_now_us());
            }
        }
        return DefWindowProcW(window,message,w,l);
    }
    void capture_loop(){
        SetThreadDescription(GetCurrentThread(),L"HiveDesk physical input");SetThreadPriority(GetCurrentThread(),THREAD_PRIORITY_ABOVE_NORMAL);
        hooks=this;capture_thread_id=GetCurrentThreadId();
        WNDCLASSW cls{};cls.lpfnWndProc=window_proc;cls.hInstance=instance;cls.lpszClassName=L"HiveDesk.InputCapture";
        const bool registered=RegisterClassW(&cls)!=0||GetLastError()==ERROR_CLASS_ALREADY_EXISTS;
        if(registered)window=CreateWindowExW(0,cls.lpszClassName,L"",0,0,0,0,0,HWND_MESSAGE,nullptr,instance,nullptr);
        if(window){
            RAWINPUTDEVICE raw{0x01,0x02,RIDEV_INPUTSINK,window};raw_available=RegisterRawInputDevices(&raw,1,sizeof(raw))!=FALSE;
            keyboard=SetWindowsHookExW(WH_KEYBOARD_LL,keyboard_proc,instance,0);mouse=SetWindowsHookExW(WH_MOUSE_LL,mouse_proc,instance,0);
        }
        {std::lock_guard lock(mutex);start_ok=window&&keyboard&&mouse;initialized=true;if(!start_ok)start_error="Could not start dedicated keyboard/mouse capture.";GetCursorPos(&pointer.point);}
        started.notify_one();
        if(start_ok){MSG msg{};while(GetMessageW(&msg,nullptr,0,0)>0){TranslateMessage(&msg);DispatchMessageW(&msg);}}
        if(keyboard)UnhookWindowsHookEx(keyboard);if(mouse)UnhookWindowsHookEx(mouse);keyboard=mouse=nullptr;
        if(raw_available){RAWINPUTDEVICE raw{0x01,0x02,RIDEV_REMOVE,nullptr};RegisterRawInputDevices(&raw,1,sizeof(raw));}
        if(window)DestroyWindow(window);window=nullptr;hooks=nullptr;
    }
    void sender_loop(){
        SetThreadDescription(GetCurrentThread(),L"HiveDesk input dispatch");SetThreadPriority(GetCurrentThread(),THREAD_PRIORITY_ABOVE_NORMAL);
        for(;;){
            CapturedInput input;Route target;
            {
                std::unique_lock lock(mutex);wake.wait(lock,[&]{return stopping||queue.size();});if(stopping)return;
                input=*queue.pop();target={state,route,state->remote&&blocked_route!=route};
                if(input.generation!=route||(!target.enabled&&input.event.kind!=InputKind::ReleaseAll))continue;
            }
            const auto now=input_now_us();
            // Tolerate transient scheduling delays; only seconds-old input
            // revokes the route rather than replaying a severely stalled burst.
            if(input.event.kind!=InputKind::ReleaseAll&&now>input.captured_us&&now-input.captured_us>SessionStallUs){fail_route(target,true);continue;}
            {
                std::lock_guard lock(mutex);
                if(input.generation!=route||(blocked_route==route&&input.event.kind!=InputKind::ReleaseAll))continue;
                delays[delay_index++%delays.size()]={now,uint32_t(std::min<uint64_t>(UINT32_MAX,now-input.captured_us))};++stats.dispatched;
            }
            // Immutable peer+session travel with the event; a UI handoff cannot
            // retarget a dequeued key onto a newly selected computer.
            try{if(send)send(target.state->peer,target.state->session,input.event,input.captured_us);}
            catch(...){if(input.event.kind!=InputKind::ReleaseAll)fail_route(target,false);}
        }
    }
};
thread_local InputCapture::Impl* InputCapture::Impl::hooks=nullptr;
InputCapture::InputCapture():impl_(std::make_unique<Impl>()){}
InputCapture::~InputCapture(){stop();}
bool InputCapture::start(HINSTANCE instance,HWND owner,UINT message,Send send,std::string& error){
    auto& s=*impl_;s.instance=instance;s.owner=owner;s.action_message=message;s.send=std::move(send);
    s.capture_thread=std::thread([&s]{s.capture_loop();});
    {std::unique_lock lock(s.mutex);s.started.wait(lock,[&]{return s.initialized;});if(!s.start_ok){error=s.start_error;return false;}}
    s.sender_thread=std::thread([&s]{s.sender_loop();});return true;
}
void InputCapture::stop(){
    auto& s=*impl_;{std::lock_guard lock(s.mutex);s.stopping=true;s.queue.clear();}s.wake.notify_all();
    if(s.capture_thread.joinable()){PostThreadMessageW(s.capture_thread_id,WM_QUIT,0,0);s.capture_thread.join();}
    if(s.sender_thread.joinable())s.sender_thread.join();
}
void InputCapture::update(InputCaptureState state){
    auto next=std::make_shared<InputCaptureState>(std::move(state));auto& s=*impl_;std::lock_guard lock(s.mutex);
    const bool same_session=next->generation==s.state->generation&&next->session==s.state->session&&next->peer==s.state->peer;
    const bool focus_changed=same_session&&next->remote!=s.state->remote;
    if(!same_session||focus_changed){++s.route;s.queue.clear();}
    // Focus loss releases keys/buttons even without another physical event.
    // Also release on regain so a rapid deactivate/activate cannot discard it.
    if(focus_changed&&next->session){s.queue.push({{InputKind::ReleaseAll},s.route,input_now_us()});s.wake.notify_one();}
    s.state=std::move(next);
}
bool InputCapture::raw_mouse_available()const{std::lock_guard lock(impl_->mutex);return impl_->raw_available;}
PhysicalPointer InputCapture::physical_pointer()const{std::lock_guard lock(impl_->mutex);return impl_->pointer;}
InputCaptureDiagnostics InputCapture::diagnostics()const{
    auto& s=*impl_;std::array<uint32_t,256> values{};size_t count=0;const auto now=input_now_us();InputCaptureDiagnostics result;
    {std::lock_guard lock(s.mutex);result=s.stats;result.coalesced=s.queue.coalesced();result.queue_depth=uint32_t(s.queue.size());result.queue_peak=uint32_t(s.queue.peak());
        for(const auto& sample:s.delays)if(sample.at&&now>=sample.at&&now-sample.at<=2000000)values[count++]=sample.us;}
    if(count){std::sort(values.begin(),values.begin()+count);result.dispatch_max_us=values[count-1];result.dispatch_p95_us=values[(count*95+99)/100-1];}return result;
}
bool InputCapture::worker_self_test(std::string& result){
    try{
        const auto require=[](bool ok,const char* message){if(!ok)throw std::runtime_error(message);};
        struct Received {std::string peer;uint64_t session;InputEvent event;};
        struct Probe {
            std::mutex mutex;std::condition_variable changed;bool block_first=false,released=false;
            std::vector<Received> received;
            InputCapture capture;
            ~Probe(){release();capture.stop();}
            void release(){{std::lock_guard lock(mutex);released=true;}changed.notify_all();}
            bool wait(size_t count){std::unique_lock lock(mutex);return changed.wait_for(lock,std::chrono::seconds(2),[&]{return received.size()>=count;});}
        };
        const auto start=[](Probe& probe){
            auto& state=*probe.capture.impl_;
            state.send=[&probe](const std::string& peer,uint64_t session,const InputEvent& event,uint64_t){
                std::unique_lock lock(probe.mutex);probe.received.push_back({peer,session,event});probe.changed.notify_all();
                if(probe.block_first&&probe.received.size()==1)probe.changed.wait(lock,[&]{return probe.released;});
            };
            state.sender_thread=std::thread([&state]{state.sender_loop();});
        };
        const auto emit=[](Probe& probe,InputEvent event,uint64_t time=0){auto& state=*probe.capture.impl_;state.emit(state.snapshot(),event,time?time:input_now_us());};
        InputCaptureState route;route.remote=true;route.peer="peer-a";route.generation=1;route.session=10;
        {
            Probe probe;probe.block_first=true;probe.capture.update(route);start(probe);
            emit(probe,{InputKind::Key,0x1e,0});
            // The calling/owner thread is waiting rather than pumping messages;
            // capture dispatch must still reach its independent sink.
            require(probe.wait(1),"input worker depended on owner message pumping");
            emit(probe,{InputKind::Relative,100,200});emit(probe,{InputKind::Key,0x1e,1});
            auto next=route;next.session=11;probe.capture.update(next);
            emit(probe,{InputKind::Key,0x30,0});probe.release();
            require(probe.wait(2),"new session did not wake input worker");
            {
                std::lock_guard lock(probe.mutex);
                require(probe.received.size()==2&&probe.received[0].peer=="peer-a"&&probe.received[0].session==10&&probe.received[0].event.a==0x1e,
                    "already dispatched event lost its original route");
                require(probe.received[1].session==11&&probe.received[1].event.a==0x30,"same-peer reconnect replayed pending old input");
            }
            next.peer="peer-b";probe.capture.update(next);emit(probe,{InputKind::Key,0x20,0});
            require(probe.wait(3),"peer handoff did not wake input worker");
            {std::lock_guard lock(probe.mutex);require(probe.received[2].peer=="peer-b"&&probe.received[2].event.a==0x20,"peer handoff retargeted old input");}
        }
        {
            Probe probe;probe.capture.update(route);start(probe);
            emit(probe,{InputKind::Key,0x1e,0},input_now_us()-400000);
            emit(probe,{InputKind::Key,0x1e,1},input_now_us()-300000);
            require(probe.wait(2),"subsecond input delay revoked route");
            {std::lock_guard lock(probe.mutex);require(probe.received.size()==2&&
                probe.received[0].event.kind==InputKind::Key&&probe.received[0].event.b==0&&
                probe.received[1].event.kind==InputKind::Key&&probe.received[1].event.b==1,"delayed key edges lost ordering");}
            require(!probe.capture.diagnostics().expired&&probe.capture.impl_->snapshot().enabled,"subsecond input blocked route");
        }
        {
            Probe probe;probe.capture.update(route);start(probe);
            emit(probe,{InputKind::Key,0x1e,0},input_now_us()-SessionStallUs-1000);
            require(probe.wait(1),"expired input did not produce emergency release");
            {std::lock_guard lock(probe.mutex);require(probe.received.size()==1&&probe.received[0].event.kind==InputKind::ReleaseAll,"expired key was replayed");}
            require(probe.capture.diagnostics().expired==1&&!probe.capture.impl_->snapshot().enabled,"expired route was not blocked");
            probe.capture.update(route);emit(probe,{InputKind::Key,0x30,0});
            {std::lock_guard lock(probe.capture.impl_->mutex);require(probe.capture.impl_->queue.size()==0,"UI snapshot reopened failed route");}
        }
        {
            Probe probe;probe.block_first=true;probe.capture.update(route);start(probe);
            emit(probe,{InputKind::Key,0x1e,0});require(probe.wait(1),"blocked worker did not enter sink");
            for(int i=0;i<257;++i)emit(probe,{InputKind::Key,0x30,i%2});
            const auto diagnostics=probe.capture.diagnostics();
            require(diagnostics.overflows==1&&diagnostics.queue_depth==1&&diagnostics.queue_peak==256,"saturated worker did not bound and replace its edge tail");
            probe.release();require(probe.wait(2),"overflow emergency release did not wake worker");
            {std::lock_guard lock(probe.mutex);require(probe.received.size()==2&&probe.received[1].event.kind==InputKind::ReleaseAll,"overflow replayed stale key transitions");}
        }
        {
            InputCaptureState geometry;geometry.windowed=true;geometry.viewer_rect={300,200,1100,650};geometry.remote_width=1920;geometry.remote_height=1080;
            InputEvent mapped{};require(Impl::map_absolute(geometry,{700,425},mapped)&&std::abs(mapped.a-32768)<100&&std::abs(mapped.b-32768)<100,"Windowed pointer did not map client origin/scale");
            require(!Impl::map_absolute(geometry,{700,180},mapped),"Title bar was treated as remote content");
            geometry.remote_width=1080;geometry.remote_height=1920;
            require(!Impl::map_absolute(geometry,{310,425},mapped)&&Impl::map_absolute(geometry,{700,425},mapped),"Windowed letterbox mapping failed");
            geometry.panel_dragging=true;require(Impl::own_surface(geometry,{1800,900}),"A dragged card leaked input outside the sidebar");
        }
        {
            Probe probe;probe.capture.update(route);start(probe);emit(probe,{InputKind::Key,0x1e,0});require(probe.wait(1),"Focus test initial input failed");
            auto inactive=route;inactive.remote=false;probe.capture.update(inactive);require(probe.wait(2),"Focus loss did not release remote input without another event");
            {std::lock_guard lock(probe.mutex);require(probe.received[1].event.kind==InputKind::ReleaseAll&&probe.received[1].peer==route.peer&&probe.received[1].session==route.session,"Focus release targeted the wrong session");}
            probe.capture.update(route);require(probe.wait(3),"Focus regain lost the release barrier");
            {std::lock_guard lock(probe.mutex);require(probe.received[2].event.kind==InputKind::ReleaseAll,"Focus regain replayed held input");}
        }
        result="input worker without hooks: owner-independent dispatch, immutable peer/session handoff, expired/overflow release, bounded wake/stop/join";return true;
    }catch(const std::exception& error){result=error.what();return false;}
}
}
