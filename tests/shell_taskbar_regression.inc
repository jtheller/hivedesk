// Included only in the isolated Shell test target, inside namespace hive.
// Drive the same input policy, owner action and fade timer as production.
// No OS input is injected and no interactive desktop is switched.
void Shell::taskbar_interaction_self_test() {
    auto& s=*impl_;
    const auto require=[](bool ok,const char* why){if(!ok)throw std::runtime_error(why);};
    USEROBJECTFLAGS flags{};DWORD bytes=0;
    require(GetUserObjectInformationW(GetProcessWindowStation(),UOI_FLAGS,&flags,sizeof(flags),&bytes)&&!(flags.dwFlags&WSF_VISIBLE),
        "Refuse taskbar interaction regression on an interactive station");
    const int width=GetSystemMetrics(SM_CXSCREEN),height=GetSystemMetrics(SM_CYSCREEN);
    WNDCLASSW c{};c.hInstance=GetModuleHandleW(nullptr);c.lpfnWndProc=DefWindowProcW;c.lpszClassName=L"Shell_TrayWnd";RegisterClassW(&c);
    const HWND tray=CreateWindowExW(0,c.lpszClassName,L"Generated taskbar",WS_POPUP|WS_VISIBLE,0,height-48,width,48,nullptr,nullptr,c.hInstance,nullptr);
    require(tray!=nullptr,"Create generated taskbar");
    const auto tray_above_panel=[&]{
        for(HWND h=GetWindow(tray,GW_HWNDNEXT);h;h=GetWindow(h,GW_HWNDNEXT))if(h==s.panel)return true;
        return false;
    };
    const auto begin=[&](bool summoned){
        const bool fresh=!s.panel_mode||s.panel_closing;
        s.show_panel(2,summoned);s.panel_summoned=summoned;s.taskbar_focus_guard=true;
        if(fresh)require(!s.taskbar_peek.active()&&!s.taskbar_peek.fading()&&!tray_above_panel(),
            "Opening Hive retained the previous taskbar state instead of covering it");
        require(s.taskbar_peek.enter(s.panel,s.viewer,tray,false),"Acquire taskbar lease");s.publish_input();
        require(tray_above_panel(),"Explicit corner trigger did not raise the taskbar");
    };
    const auto click=[&](POINT p,HWND target){
        const auto state=s.input_state();
        require(dismiss_panel_on_mouse_down(state,p,target),"Local taskbar mode swallowed Hive outside-click dismissal");
        SendMessageW(s.owner,CaptureActionMessage,WPARAM(CaptureAction::HidePanelAnimated),LPARAM(state.generation+1));
        require(!s.panel_closing,"A stale input route dismissed the current Hive");
        // This is the real generation-checked owner handler, not hide_panel().
        SendMessageW(s.owner,CaptureActionMessage,WPARAM(CaptureAction::HidePanelAnimated),LPARAM(state.generation));
        require(s.panel_closing&&s.taskbar_peek.active(),"Dismissing Hive restored taskbar before its native click");
        s.update_panel_pointer({20,height-20},GetTickCount64());
        require(s.panel_closing,"Start overlap canceled the sidebar dismissal");
        SendMessageW(s.owner,WM_TIMER,2,0);
        require(!(GetWindowLongPtrW(s.panel,GWL_STYLE)&WS_VISIBLE)&&!s.panel_mode,"Hive stayed visible after outside click/fade");
        require(s.taskbar_peek.active()&&s.input_state().local_shell&&!s.input_state().remote,
            "Fade completion revoked local taskbar input");
        require(!dismiss_panel_on_mouse_down(s.input_state(),p,target),"Hidden Hive keeps requesting dismissals");
        require(GetWindowLongPtrW(tray,GWL_EXSTYLE)&WS_EX_TOPMOST,"Fade completion lowered the native taskbar");
        s.update_panel_pointer({400,400},GetTickCount64());
        require(s.taskbar_peek.active(),"Periodic sampling revoked taskbar mode after hiding Hive");
    };
    require(!s.remote&&!(GetWindowLongPtrW(s.viewer,GWL_STYLE)&WS_VISIBLE),"Fixture must start on the local desktop with no viewer");
    for(bool summoned:{false,true}){
        begin(summoned);
        require(!dismiss_panel_on_mouse_down(s.input_state(),{100,100},s.panel),"Clicking a Hive card dismisses it");
        auto dragging=s.input_state();dragging.panel_dragging=true;
        require(!dismiss_panel_on_mouse_down(dragging,{400,400},tray),"Dragging a card outside dismissed Hive");
        click({400,400},nullptr);
        // Reopening must not need an intervening connection or app restart.
        begin(summoned);
        click({20,height-20},tray); // Start overlaps the sidebar rectangle.
        begin(summoned);SendMessageW(s.owner,WM_HOTKEY,s.hotkey_id,0);SendMessageW(s.owner,WM_TIMER,2,0);
        require(s.taskbar_peek.active()&&!s.panel_mode,"Closing Hive restored taskbar state before the next opening");
        SendMessageW(s.owner,WM_HOTKEY,s.hotkey_id,0);
        require(!s.taskbar_peek.active()&&s.panel_mode==2&&!tray_above_panel(),"Hotkey reopening did not start clean");
        s.hide_panel();
    }
    begin(false);
    s.update_panel_pointer({400,400},GetTickCount64());
    s.update_panel_pointer({400,400},GetTickCount64()+251);
    require(s.panel_closing&&s.taskbar_peek.active(),"Pointer-opened Hive no longer fades out on leaving its panel");
    SendMessageW(s.owner,WM_TIMER,2,0);
    require(!s.panel_mode&&s.taskbar_peek.active(),"Hover dismissal reset the taskbar lease");
    // Mouse entry starts the same clean cycle as the hotkey. Expanding its
    // preview is still part of that cycle, not an independent taskbar trigger.
    s.show_panel(1);require(!s.taskbar_peek.active(),"Mouse preview retained the previous taskbar lease");
    s.show_panel(2);require(!tray_above_panel(),"Expanding Hive did not cover the taskbar");
    begin(true);
    s.hide_panel(true,true);require(s.panel_closing&&s.taskbar_peek.active(),"Begin fade reversal fixture");
    SendMessageW(s.owner,WM_HOTKEY,s.hotkey_id,0);
    require(!s.taskbar_peek.active()&&!s.taskbar_peek.fading()&&!tray_above_panel(),"Reopening during fade retained taskbar state");
    SendMessageW(s.owner,WM_TIMER,2,0);
    require(s.panel_mode==2&&!s.panel_closing,"Old dismissal timer hid the newly opened Hive");
    // Reopening with the pointer still in the bottom corner must remain clean
    // until the next entry into that corner. Drive the actual timer path.
    s.begin_panel_cycle({0,height-1});
    s.update_panel_pointer({0,height-1},GetTickCount64());
    require(!s.taskbar_peek.active()&&!s.taskbar_corner_armed,"Stationary corner pointer immediately undid the clean opening");
    s.update_panel_pointer({100,height-1},GetTickCount64());
    s.update_panel_pointer({0,height-1},GetTickCount64());
    require(s.taskbar_peek.active()&&tray_above_panel(),"Fresh corner entry did not expose the taskbar");
    s.update_panel_pointer({100,height-100},GetTickCount64());
    require(s.taskbar_peek.active(),"Leaving the taskbar restored clean state before the next opening");
    s.hide_panel(false,true);
    begin(true);
    s.remote=true;s.active_id="taskbar-fixture";s.input_session=71;s.publish_input();
    click({20,height-20},tray);
    ShowWindow(s.viewer,SW_SHOWNOACTIVATE);
    SendMessageW(s.viewer,WM_ACTIVATE,WA_ACTIVE,0);
    require(!s.taskbar_peek.active(),"Viewer activation did not release the taskbar lease");
    s.local(false);DestroyWindow(tray);
    require(!s.taskbar_peek.active()&&!s.panel_mode,"Return-local cleanup retained the popup");
}
