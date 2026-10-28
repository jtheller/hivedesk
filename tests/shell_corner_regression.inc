// Included only by the isolated Shell regression target. No OS input injection.
void Shell::quick_swap_interaction_self_test() {
    auto& s=*impl_;
    const auto require=[](bool ok,const char* why){if(!ok)throw std::runtime_error(why);};
    USEROBJECTFLAGS flags{};DWORD bytes=0;
    require(GetUserObjectInformationW(GetProcessWindowStation(),UOI_FLAGS,&flags,sizeof(flags),&bytes)&&!(flags.dwFlags&WSF_VISIBLE),
        "Refuse corner regression on an interactive station");
    require(s.peers.size()>=2,"Corner fixture needs two computers");
    const auto first=s.peers[0].id,second=s.peers[1].id;
    const auto saved_callbacks=s.callbacks;const auto saved_dpi=s.dpi;
    struct Restore {Impl& s;ShellCallbacks callbacks;int dpi;~Restore(){s.callbacks=std::move(callbacks);s.dpi=dpi;}} restore{s,saved_callbacks,saved_dpi};
    unsigned selections=0,returns=0;std::string selected;
    s.callbacks.select_peer=[&](const std::string& id){++selections;selected=id;};
    s.callbacks.return_local=[&]{++returns;};
    const auto click=[&](int x,int y){SendMessageW(s.panel,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(x,y));SendMessageW(s.panel,WM_LBUTTONUP,0,MAKELPARAM(x,y));};
    for(const auto dpi:{96,144,192}){
        s.local(false);s.previous_id.clear();s.dpi=dpi;
        const auto before=selections,local_before=returns;
        s.update_panel_pointer({0,0},GetTickCount64());
        require(s.panel_mode==1&&s.card_at({0,0})==1,"Exact screen corner does not target the compact preview");
        require(s.card_at({-1,0})==-1&&s.card_at({0,-1})==-1,"Corner hit target escaped primary-screen bounds");
        click(0,0); // No pointer move into the inset card, no animation wait.
        require(selections==before+1&&selected==first,"Corner click did not select the previewed computer exactly once");
        s.show_remote_ui(first,L"Corner first",1);
        s.update_panel_pointer({0,0},GetTickCount64());click(0,0);
        require(returns==local_before+1&&s.active_id.empty(),"Corner click could not swap back to the local desktop");
        s.update_panel_pointer({0,0},GetTickCount64());click(1,1);
        require(selections==before+2&&selected==first,"Corner click did not return to the previous remote computer");
        s.show_remote_ui(first,L"Corner first",2);s.select(second);s.show_remote_ui(second,L"Corner second",3);
        s.update_panel_pointer({0,0},GetTickCount64());click(0,0);
        require(selections==before+4&&selected==first,"Remote-to-remote corner swap ignored the previous computer");
        s.local(false);s.update_panel_pointer({0,0},GetTickCount64());
        const auto cancelled=selections;
        SendMessageW(s.panel,WM_LBUTTONDOWN,MK_LBUTTON,0);SendMessageW(s.panel,WM_CANCELMODE,0,0);SendMessageW(s.panel,WM_LBUTTONUP,0,0);
        require(selections==cancelled,"Cancelled corner press selected a computer");
        SendMessageW(s.panel,WM_LBUTTONDOWN,MK_LBUTTON,0);SendMessageW(s.panel,WM_LBUTTONUP,0,MAKELPARAM(s.panel_width+20,0));
        require(selections==cancelled,"Corner press released outside selected a computer");
        s.update_panel_pointer({0,s.px(40)},GetTickCount64());
        require(s.panel_mode==2&&s.card_at({0,0})==-1,"Down-edge expansion or full-list padding changed");
        click(0,0);require(selections==cancelled,"Full Hive corner unexpectedly selected a card");
    }
    s.local(false);s.previous_id.clear();
}
