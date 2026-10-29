void Shell::nested_return_self_test(){
    auto& s=*impl_;
    const auto require=[](bool ok,const char* why){if(!ok)throw std::runtime_error(why);};
    USEROBJECTFLAGS flags{};DWORD bytes=0;
    require(GetUserObjectInformationW(GetProcessWindowStation(),UOI_FLAGS,&flags,sizeof(flags),&bytes)&&!(flags.dwFlags&WSF_VISIBLE),"Nested regression requires isolated station");
    auto saved=s.callbacks;const auto peers=s.peers;
    struct Cleanup{Impl& s;ShellCallbacks callbacks;std::vector<Peer> peers;~Cleanup(){s.local(false);s.callbacks=std::move(callbacks);s.peers=std::move(peers);}} cleanup{s,saved,peers};
    unsigned closes=0,sends=0;ReturnRequest sent;ReturnReply reply;
    s.callbacks.return_local=[&]{++closes;};
    s.callbacks.return_request=[&](const std::string&,const ReturnRequest& r){++sends;sent=r;return true;};
    s.callbacks.return_reply=[&](const std::string&,const ReturnReply& r){reply=r;return true;};
    const auto viewing=[&](uint64_t session){
        s.local(false);s.end_peek(false);s.taskbar_focus_guard=false;
        s.peers={{"nested-child","Child","",47980,true,FeatureNestedReturn}};
        ++s.generation;s.selection_armed=true;s.remote=true;s.active_id=s.pending_id="nested-child";s.input_session=session;s.publish_input();
    };
    viewing(11);s.return_begin();require(s.nested_return.busy()&&s.input_state().local_shell,"pending return did not yield input authority");
    const auto first=sent;s.return_begin();require(sends==1,"rapid repeat queued another unwind");
    returned("nested-child",{11,first.id,ReturnResult::Returned});
    require(closes==0&&s.remote&&!s.nested_return.busy()&&!s.input_state().local_shell,"successful child unwind also closed parent or stranded input");
    s.return_begin();returned("nested-child",{11,sent.id,ReturnResult::Leaf});
    require(closes==1&&!s.remote&&s.input_session==0,"leaf return did not release local viewer");
    viewing(12);bool authorized=true;
    return_one("parent",{77,1,15},[&]{return authorized;});const auto old=sent;
    authorized=false;returned("nested-child",{12,old.id,ReturnResult::Leaf});
    require(closes==1&&s.remote&&reply.result==ReturnResult::Stale,"revoked inbound controller closed a viewer");
    authorized=true;return_one("parent",{77,2,15},[&]{return authorized;});
    const auto old2=sent;++s.generation;
    returned("nested-child",{12,old2.id,ReturnResult::Leaf});
    require(closes==1&&reply.result==ReturnResult::Stale,"queued reply closed a new generation");
    const auto count=sends;return_one("parent",{77,2,15},[]{return true;});
    require(sends==count&&reply.result==ReturnResult::Stale,"duplicate command restarted an unwind");
    return_one("parent",{77,3,0},[]{return true;});require(reply.result==ReturnResult::Limit&&s.remote,"hop limit closed a view");
    return_one("parent",{77,4,15},[]{return true;});
    return_one("cycle",{88,1,14},[]{return true;});require(reply.result==ReturnResult::Busy,"cycle was not bounded by busy admission");
    const auto pending=sent;s.local(true);
    returned("nested-child",{12,pending.id,ReturnResult::Leaf});require(closes==2&&!s.remote,"late reply repeated emergency return");
    return_one("parent",{77,5,15},[]{return true;});require(reply.result==ReturnResult::Leaf&&closes==2,"leaf node unwound inbound stream");
    viewing(13);s.peers[0].features=0;s.return_begin();require(closes==3&&!s.remote,"legacy boundary lost old local escape");
    viewing(0);s.return_begin();require(closes==4&&!s.remote,"connecting selection could not be cancelled");
    viewing(14);s.return_begin();
    s.return_complete(s.nested_return.tick(GetTickCount64()+10001,s.return_view(),true));
    require(s.remote&&!s.nested_return.busy()&&!s.input_state().local_shell&&closes==4,"timeout closed or stranded outer desktop");
    viewing(15);s.windowed=true;s.remote_width=1920;s.remote_height=1080;
    SetWindowPos(s.viewer,HWND_TOP,300,200,800,500,SWP_SHOWWINDOW);SetForegroundWindow(s.viewer);s.publish_input();
    auto relay_state=s.input_state();relay_state.remote=true;s.input_capture.update(relay_state);
    const auto bounds=s.viewer_client();const POINT center{(bounds.left+bounds.right)/2,(bounds.top+bounds.bottom)/2};
    const auto absolute=[&](POINT p){return InputEvent{InputKind::Absolute,int32_t((int64_t(p.x)*65536)/GetSystemMetrics(SM_CXSCREEN)),int32_t((int64_t(p.y)*65536)/GetSystemMetrics(SM_CYSCREEN))};};
    auto mapped=route_remote(501,absolute(center),true,true);
    require(mapped.forward&&mapped.local&&std::abs(mapped.event.a-32768)<300&&std::abs(mapped.event.b-32768)<300,"Nested window center/letterbox coordinate mapping");
    // Local broker positioning has not run. Button hit testing must use the
    // preceding authenticated absolute packet, not the old Windows cursor.
    mapped=route_remote(501,{InputKind::Button,1,0},true,true);require(mapped.forward&&!mapped.local,"Nested button used stale intermediate OS position");
    mapped=route_remote(501,absolute({0,0}),true,true);require(mapped.forward&&mapped.event.a==0&&mapped.event.b==0,"Nested drag escaped its captured viewer");
    mapped=route_remote(501,{InputKind::Button,1,1},true,true);require(mapped.forward&&!mapped.local,"Nested drag release lost outside window");
    mapped=route_remote(501,absolute({0,0}),true,true);require(!mapped.forward&&mapped.local,"Local desktop pointer forwarded to nested viewer");
    mapped=route_remote(501,{InputKind::Key,30,0},false,true);require(!mapped.forward&&mapped.local,"Secure desktop input forwarded behind lock screen");
    route_remote(501,{InputKind::ReleaseAll},true,true);s.windowed=false;
}
