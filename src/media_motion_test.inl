// Included within namespace hive after the private GPU classes. Developer-only:
// generated pixels, hidden window, no capture, input injection or network traffic.
bool media_motion_self_test(std::string& report) {
    struct HiddenWindow {HWND value=nullptr;~HiddenWindow(){if(value)DestroyWindow(value);}} window;
    std::string phase="motion fixture initialization";
    try {
        constexpr uint32_t width=2560,height=1440,frames=72,warmup=8;
        constexpr wchar_t class_name[]=L"HiveDeskMotionTest";
        const auto began=Clock::now();
        WNDCLASSW cls{};cls.lpfnWndProc=DefWindowProcW;cls.hInstance=GetModuleHandleW(nullptr);cls.lpszClassName=class_name;
        if(!RegisterClassW(&cls)&&GetLastError()!=ERROR_CLASS_ALREADY_EXISTS)throw std::runtime_error("Cannot register hidden motion window.");
        window.value=CreateWindowExW(0,class_name,L"HiveDesk hidden motion test",WS_POPUP,-32000,-32000,width,height,nullptr,nullptr,cls.hInstance,nullptr);
        if(!window.value)throw std::runtime_error("Cannot create hidden motion window.");
        Renderer renderer(window.value);
        Image source{width,height,std::vector<uint8_t>(size_t(width)*height*4)};
        std::vector<uint8_t> desktop(source.bgra.size()),document(source.bgra.size()),cut(source.bgra.size());
        // Fixed detailed pages, regenerated identically for every invocation.
        // Textlike strokes/chroma edges exercise motion prediction; quantitative
        // text/chroma fidelity remains covered by the separate --auto-quality.
        for(uint32_t y=0;y<height;++y)for(uint32_t x=0;x<width;++x){
            const size_t at=(size_t(y)*width+x)*4;
            auto* a=desktop.data()+at;auto* b=document.data()+at;auto* c=cut.data()+at;
            a[0]=uint8_t(35+(x/16+y/16)%50);a[1]=uint8_t(25+(x/32)%60);a[2]=uint8_t(20+(y/16)%65);a[3]=255;
            const bool stroke=y%24>=5&&y%24<17&&x%12<8&&((x/12+y/24)%7!=0);
            const bool edge=x>width-128&&(x&1);
            b[0]=edge?220:stroke?30:245;b[1]=edge?35:stroke?40:245;b[2]=edge?35:stroke?70:245;b[3]=255;
            const uint32_t detail=(x*17+y*29+(x*y)%251)%256;
            c[0]=uint8_t(((x/3+y/3)&1)?detail:255-detail);
            c[1]=uint8_t((x/5+y/7)*11);c[2]=uint8_t((x/7-y/5)*13);c[3]=255;
        }
        const auto generate=[&](uint32_t section,uint32_t frame){
            if(section==0){
                source.bgra=desktop;
                constexpr uint32_t window_width=1536,window_height=920;
                const uint32_t left=32+(frame*19)%(width-window_width-64),top=32+(frame*11)%(height-window_height-64);
                for(uint32_t y=0;y<window_height;++y){
                    auto* row=source.bgra.data()+(size_t(top+y)*width+left)*4;
                    std::memcpy(row,document.data()+size_t(y)*width*4,size_t(window_width)*4);
                    if(y<32)for(uint32_t x=0;x<window_width;++x){row[x*4]=160;row[x*4+1]=75;row[x*4+2]=35;}
                }
            }else{
                const auto& page=section==1?document:((frame/6)%2?cut:document);
                for(uint32_t y=0;y<height;++y){
                    const auto row=section==1?(y+frame*13)%height:y;
                    std::memcpy(source.bgra.data()+size_t(y)*width*4,page.data()+size_t(row)*width*4,size_t(width)*4);
                }
            }
            // Large uniform corner marker survives lossy quantization. Each
            // phase's final frame is verified from the actual rendered bitmap.
            for(uint32_t y=0;y<64;++y)for(uint32_t x=0;x<64;++x){
                auto* p=source.bgra.data()+(size_t(y)*width+x)*4;
                p[0]=uint8_t(200-section*60);p[1]=uint8_t(24+(frame%4)*64);p[2]=uint8_t(40+section*80);p[3]=255;
            }
        };
        auto texture=renderer.upload_for_test(source);
        ComPtr<ID3D11Device> device;texture->GetDevice(&device);
        ComPtr<ID3D11DeviceContext> context;device->GetImmediateContext(&context);
        D3D11_QUERY_DESC query_desc{};query_desc.Query=D3D11_QUERY_EVENT;
        ComPtr<ID3D11Query> uploaded;hr(device->CreateQuery(&query_desc,&uploaded),"Create motion upload fence");
        struct Metrics {
            std::vector<uint64_t> stage[5];
            uint64_t bytes=0,max_frame_bytes=0,qp_sum=0;
            uint32_t count=0,min_qp=51,max_qp=0,presented=0;
        } metrics[3];
        const char* names[]{"window_drag","scroll","hard_cuts"};
        Encoder encoder(width,height,StreamSettings{0,0,100000,1},renderer.adapter());
        uint32_t decoded=0,expected_sequence=0,section=0,frame_index=0;
        bool measured=false,verify=false;
        uint64_t callback_us=0;
        uint32_t render_us=0,present_us=0;
        bool presented=false;
        Decoder decoder([&](uint64_t yuv,size_t pitch,uint32_t w,uint32_t h,HiveGpu* gpu){
            const auto callback_begin=now_us();
            if(w!=width||h!=height||decoded!=expected_sequence)throw std::runtime_error("Motion decode dimensions/count identity mismatch.");
            Image proof;
            renderer.draw(yuv,pitch,w,h,gpu,verify?&proof:nullptr,&render_us,&present_us,&presented);
            if(verify){
                if(proof.width!=width||proof.height!=height)throw std::runtime_error("Motion proof changed render dimensions.");
                const size_t marker=(size_t(32)*width+32)*4;
                for(size_t channel=0;channel<3;++channel)
                    if(std::abs(int(proof.bgra[marker+channel])-int(source.bgra[marker+channel]))>24)
                        throw std::runtime_error("Motion phase/frame marker did not reconstruct in the rendered picture.");
            }
            // A hidden Present may be occluded/busy. Never retain a retry queue
            // in this serial diagnostic or describe Present calls as scanout.
            renderer.cancel_pending();++decoded;callback_us=now_us()-callback_begin;
        },renderer.adapter());
        for(uint32_t sequence=0;sequence<warmup+frames*3;++sequence){
            if(Clock::now()-began>std::chrono::seconds(30))throw std::runtime_error("Generated motion test exceeded its 30-second budget.");
            measured=sequence>=warmup;section=measured?(sequence-warmup)/frames:0;
            frame_index=measured?(sequence-warmup)%frames:0;phase=measured?names[section]:"motion warmup";
            generate(section,frame_index);
            // Generation and both CPU/GPU upload complete outside stage timing.
            context->UpdateSubresource(texture.Get(),0,nullptr,source.bgra.data(),width*4,0);
            context->End(uploaded.Get());context->Flush();
            for(;;){
                const auto ready=context->GetData(uploaded.Get(),nullptr,0,0);
                if(ready==S_OK)break;hr(ready,"Wait for motion upload");
                if(Clock::now()-began>std::chrono::seconds(30))throw std::runtime_error("Motion upload exceeded its time budget.");
                Sleep(0);
            }
            const auto captured=now_us();
            if(!encoder.submit_texture(texture.Get(),sequence,sequence==0,captured))throw std::runtime_error("Motion encoder rejected its idle slot.");
            Encoder::Completion completion;
            if(!encoder.receive(completion)||completion.frame.sequence!=sequence||completion.frame.capture_time_us!=captured||completion.frame.keyframe!=(sequence==0))
                throw std::runtime_error("Motion encoding reordered/lost a frame or inserted an unrequested IDR.");
            if(completion.average_qp<AutoRateMinQp||completion.average_qp>AutoRateMaxQp)
                throw std::runtime_error("Motion automatic CBR escaped its configured QP bounds.");
            expected_sequence=sequence;verify=measured&&frame_index==frames-1;
            callback_us=0;render_us=present_us=0;presented=false;
            const auto decode_begin=now_us();decoder.feed(completion.frame);const auto decode_elapsed=now_us()-decode_begin;
            if(decoded!=sequence+1)throw std::runtime_error("Motion decoder buffered/lost a submitted frame.");
            if(measured){
                auto& m=metrics[section];++m.count;m.bytes+=completion.frame.data.size();
                m.max_frame_bytes=std::max<uint64_t>(m.max_frame_bytes,completion.frame.data.size());
                m.qp_sum+=completion.average_qp;m.min_qp=std::min(m.min_qp,completion.average_qp);m.max_qp=std::max(m.max_qp,completion.average_qp);
                if(presented)++m.presented;
                // Proof readback affects GPU synchronization and draw time;
                // omit each phase's final proof frame from all stage samples.
                if(!verify){
                    m.stage[0].push_back(completion.color_us);m.stage[1].push_back(completion.nvenc_us);
                    m.stage[2].push_back(decode_elapsed>=callback_us?decode_elapsed-callback_us:0);
                    m.stage[3].push_back(render_us);m.stage[4].push_back(present_us);
                }
            }
        }
        encoder.finish();Encoder::Completion extra;
        if(encoder.receive(extra))throw std::runtime_error("Motion encoder emitted an unsubmitted extra frame.");
        std::ostringstream text;text<<std::fixed<<std::setprecision(3);
        text<<"PASS generated motion: "<<width<<'x'<<height<<" HEVC444 automatic CBR "<<encoder.bitrate_bps()/1000000.0
            <<" Mbps target at nominal100Hz, QP"<<AutoRateMinQp<<".."<<AutoRateMaxQp<<"; "<<warmup<<" warmup + "<<frames*3
            <<" ordered encoded/decoded frames, only initial IDR, three rendered identity markers verified.\n";
        const char* stages[]{"color","NVENC_residence","decode_without_callback","convert_and_draw","hidden_Present_call"};
        for(uint32_t index=0;index<3;++index){
            auto& m=metrics[index];
            text<<names[index]<<": frames="<<m.count<<", bytes="<<m.bytes<<", max_frame_bytes="<<m.max_frame_bytes
                <<", QP_mean/min/max="<<double(m.qp_sum)/m.count<<'/'<<m.min_qp<<'/'<<m.max_qp<<", Present_S_OK="<<m.presented<<'/'<<m.count<<"\n";
            for(uint32_t stage=0;stage<5;++stage){
                auto values=m.stage[stage];std::sort(values.begin(),values.end());
                if(values.size()!=frames-1)throw std::runtime_error("Motion stage sample count mismatch.");
                text<<"  "<<stages[stage]<<" ms p50/p95/max="<<values[values.size()/2]/1000.0<<'/'
                    <<values[(values.size()-1)*95/100]/1000.0<<'/'<<values.back()/1000.0<<"\n";
            }
        }
        text<<"Serial stage observations; CPU generation/GPU upload, warmup and three proof-readback frames excluded from stage samples. "
            <<"No capture, LAN, controller adaptation, asynchronous throughput or physical scanout measurement. Hidden/offscreen only. Total elapsed="
            <<std::chrono::duration<double>(Clock::now()-began).count()<<"s.";
        report=text.str();return true;
    }catch(const std::exception& error){report=std::string("FAIL: ")+phase+": "+error.what();return false;}
}
