// Actual Shell dispatch/selection/position path, with only the final OS
// injection replaced. Run solely on the invisible test window station.
void Shell::cursor_sync_self_test(){
    auto& s=*impl_;
    const auto require=[](bool ok,const char* why){if(!ok)throw std::runtime_error(why);};
    USEROBJECTFLAGS flags{};DWORD bytes=0;
    require(GetUserObjectInformationW(GetProcessWindowStation(),UOI_FLAGS,&flags,sizeof(flags),&bytes)&&!(flags.dwFlags&WSF_VISIBLE),"Cursor regression requires isolated station");
    s.local(false);s.hide_panel();s.end_peek(false);s.taskbar_focus_guard=false;
    s.remote=true;s.relative_mode=false;s.windowed=false;s.active_id="cursor-fixture";
    s.pending_id=s.active_id;s.input_session=41;s.remote_width=1920;s.remote_height=1080;s.last_local_motion=0;
    const auto publish=[&]{auto route=s.input_state();route.remote=!s.taskbar_focus_guard;s.input_capture.update(route);};
    s.cursor_position_gate.reset(41);publish();unsigned injections=0;INPUT last{};
    POINT fixture_point{100,100};bool fixture_focus=true;
    s.cursor_context_test=[&](POINT& point,bool& focused){point=fixture_point;focused=fixture_focus;};
    s.cursor_position_test_sink=[&](const INPUT& input){++injections;last=input;};
    struct Cleanup{Impl& s;~Cleanup(){s.cursor_position_test_sink={};s.cursor_context_test={};s.local(false);}} cleanup{s};
    CursorState c;c.session=41;c.position_revision=1;
    require(read_cursor_shape(LoadCursorW(nullptr,IDC_IBEAM),c),"Read generated cursor shape");
    update_cursor(s.active_id,c);require(!injections&&s.remote_cursor_received&&s.remote_cursor,"Initial snapshot warped or lost cursor shape");
    c.position_valid=true;c.position_revision=2;c.x=c.y=32768;
    update_cursor(s.active_id,c);require(injections==1&&last.mi.dwExtraInfo==InputMarker,"Current host movement not injected with echo marker");
    require(last.mi.dx>0&&last.mi.dx<65536&&last.mi.dy>0&&last.mi.dy<65536&&(last.mi.dwFlags&MOUSEEVENTF_VIRTUALDESK),"Cursor coordinate mapping escaped virtual desktop");
    c.position_revision=1;c.x=c.y=0;update_cursor(s.active_id,c);require(injections==1,"Older position warped to corner");
    c.position_revision=3;c.visible=false;update_cursor(s.active_id,c);require(injections==2&&!s.remote_cursor_visible,"Current scoped visibility update lost");
    // Queue a callback from the socket thread, then change selection before UI drain.
    c.position_revision=4;c.visible=true;
    std::thread delayed([&]{update_cursor(s.active_id,c);});delayed.join();++s.generation;
    MSG message{};while(PeekMessageW(&message,s.owner,UiTask,UiTask,PM_REMOVE))DispatchMessageW(&message);
    require(injections==2&&!s.remote_cursor_visible,"Retired UI-generation cursor callback applied");
    s.input_session=42;s.cursor_position_gate.reset(42);s.remote_pointer={};publish();
    c.position_revision=999;update_cursor(s.active_id,c);require(injections==2,"Old same-peer session moved the cursor");
    c.session=42;c.position_revision=1;c.position_valid=false;update_cursor(s.active_id,c);require(injections==2,"Reconnect baseline warped");
    c.position_revision=2;c.position_valid=true;c.x=c.y=32768;update_cursor(s.active_id,c);require(injections==3,"Reconnect lost valid host movement");
    s.taskbar_focus_guard=true;publish();c.position_revision=3;update_cursor(s.active_id,c);
    require(injections==3,"Local taskbar control allowed remote cursor correction");
    s.taskbar_focus_guard=false;publish();
    c.session=0;c.position_revision=1000;c.visible=false;update_cursor(s.active_id,c);
    require(injections==3&&!s.remote_cursor_visible,"Legacy shape-only compatibility warped or lost visibility");
    s.relative_mode=true;c.session=42;c.position_revision=4;update_cursor(s.active_id,c);require(injections==3,"Relative mode accepted absolute correction");
    s.relative_mode=false;fixture_point={-100,100};c.position_revision=5;update_cursor(s.active_id,c);require(injections==3,"Host cursor pulled pointer from left monitor");
    fixture_point={GetSystemMetrics(SM_CXSCREEN)+100,100};c.position_revision=6;update_cursor(s.active_id,c);require(injections==3,"Host cursor pulled pointer from right monitor");
    fixture_point={100,100};fixture_focus=false;c.position_revision=7;update_cursor(s.active_id,c);require(injections==3,"Unfocused viewer stole local cursor");
    fixture_focus=true;c.position_revision=8;update_cursor(s.active_id,c);require(injections==4,"Cursor sync failed to resume inside focused viewer");
    RECT tiny{100,100,101,101};ClipCursor(&tiny);s.confine_mouse();RECT clip{};GetClipCursor(&clip);
    require(clip.right-clip.left>1&&clip.bottom-clip.top>1,"Absolute mode retained pointer confinement");
    std::puts("PASS cursor Shell: baseline, marked coordinate injection, shape/visibility, delayed UI callback, same-peer reconnect and legacy/relative safety; no OS input injected");
}
