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
    const auto saved_peers=s.peers;
    struct Restore {Impl& s;ShellCallbacks callbacks;int dpi;std::vector<Peer> peers;~Restore(){s.callbacks=std::move(callbacks);s.dpi=dpi;s.peers=std::move(peers);}} restore{s,saved_callbacks,saved_dpi,saved_peers};
    s.peers[0].features|=FeatureLivePeek;
    unsigned selections=0,returns=0;std::string selected;
    s.callbacks.select_peer=[&](const std::string& id){++selections;selected=id;};
    s.callbacks.return_local=[&]{++returns;};
    const auto click=[&](int x,int y){SendMessageW(s.panel,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(x,y));SendMessageW(s.panel,WM_LBUTTONUP,0,MAKELPARAM(x,y));};
    for(const auto dpi:{96,144,192}){
        s.local(false);s.previous_id.clear();s.dpi=dpi;
        const auto before=selections,local_before=returns;
        s.update_panel_pointer({0,0},GetTickCount64());
        require(s.panel_mode==1&&s.click_card_at({0,0})==1,"Exact screen corner does not target the compact preview");
        require(s.click_card_at({-1,0})==-1&&s.click_card_at({0,-1})==-1,"Corner hit target escaped primary-screen bounds");
        s.update_panel_pointer({0,0},GetTickCount64()+1000);
        SendMessageW(s.panel,WM_MOUSEMOVE,0,0);
        require(s.peek_id.empty(),"Stationary hot corner triggered Peek");
        for(const POINT inset: {POINT{s.px(11),s.px(12)},POINT{s.px(12),s.px(11)}}){
            s.update_panel_pointer(inset,GetTickCount64());
            SendMessageW(s.panel,WM_MOUSEMOVE,0,MAKELPARAM(inset.x,inset.y));
            require(s.peek_id.empty(),"Card padding triggered Peek");
        }
        const POINT card{s.px(13),s.px(13)};
        s.update_panel_pointer(card,GetTickCount64());
        require(s.peek_id==first,"Timer hover inside the actual card did not enable Peek");
        SendMessageW(s.panel,WM_MOUSEMOVE,0,0);
        require(s.peek_id.empty(),"Moving back to the corner retained Peek");
        SendMessageW(s.panel,WM_MOUSEMOVE,0,MAKELPARAM(card.x,card.y));
        require(s.peek_id==first,"Mouse hover inside the actual card did not enable Peek");
        s.update_panel_pointer({0,0},GetTickCount64());
        require(s.peek_id.empty(),"Corner timer retained the card's previous Peek");
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
