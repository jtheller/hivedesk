// Generated pixels and hidden HWND only; never capture or inject desktop input.
bool media_async_decode_self_test(std::string& report){
    struct Window {HWND value=nullptr;~Window(){if(value)DestroyWindow(value);}} window;
    std::string phase="setup";
    try{
        const auto require=[](bool pass,const char* why){if(!pass)throw std::runtime_error(why);};
        constexpr wchar_t name[]=L"HiveDeskAsyncHiddenTest";
        WNDCLASSW cls{};cls.lpfnWndProc=DefWindowProcW;cls.hInstance=GetModuleHandleW(nullptr);cls.lpszClassName=name;
        if(!RegisterClassW(&cls)&&GetLastError()!=ERROR_CLASS_ALREADY_EXISTS)throw std::runtime_error("Register hidden async window");
        window.value=CreateWindowExW(0,name,L"HiveDesk hidden async test",WS_POPUP,-32000,-32000,512,384,nullptr,nullptr,cls.hInstance,nullptr);
        require(window.value&&!IsWindowVisible(window.value),"Create hidden async window");
        Renderer renderer(window.value,true);
        const auto image=[](uint32_t w,uint32_t h,uint8_t red){Image result{w,h,std::vector<uint8_t>(size_t(w)*h*4)};
            for(size_t i=0;i<result.bgra.size();i+=4){result.bgra[i]=40;result.bgra[i+1]=90;result.bgra[i+2]=red;result.bgra[i+3]=255;}return result;};
        std::vector<EncodedFrame> frames;
        {Encoder encoder(256,192,StreamSettings{60,100},renderer.adapter());
            for(unsigned i=0;i<8;++i)frames.push_back(encoder.encode(image(256,192,uint8_t(60+i*15)),i,i==0));}
        EncodedFrame resized;
        {Encoder encoder(320,240,StreamSettings{60,100},renderer.adapter());resized=encoder.encode(image(320,240,190),8,true);}
        struct Gate {
            std::mutex mutex;std::condition_variable changed;bool entered=false,released=false;
            void block(){std::unique_lock lock(mutex);entered=true;changed.notify_all();changed.wait(lock,[&]{return released;});}
            bool await(){std::unique_lock lock(mutex);return changed.wait_for(lock,std::chrono::seconds(2),[&]{return entered;});}
            void release(){std::lock_guard lock(mutex);released=true;changed.notify_all();}
        };
        phase="bounded map overlap and format change";
        {
            Gate first;std::vector<unsigned> observed;
            Decoder decoder({},renderer.adapter(),true);
            struct Cleanup {Gate& gate;Decoder& decoder;~Cleanup(){gate.release();decoder.stop_async();}} cleanup{first,decoder};
            const auto output=[&](unsigned sequence){Decoder::AsyncOutput result;
                result.prepare=[&,sequence](uint64_t yuv,size_t pitch,uint32_t w,uint32_t h,HiveGpu* gpu){
                    if(sequence==0)first.block();Image decoded{w,h,std::vector<uint8_t>(size_t(w)*h*4)};
                    if(!hive_gpu_decode_host(gpu,yuv,pitch,w,h,decoded.bgra.data()))throw std::runtime_error(hive_gpu_error(gpu));
                    require(w==256&&h==192&&std::abs(int(decoded.bgra[2])-int(60+sequence*15))<=4,"Wrong decoded picture/surface");observed.push_back(sequence);
                };return result;};
            require(decoder.feed_async(frames[0],output(0))&&first.await(),"Mapping worker did not start");
            auto second=std::async(std::launch::async,[&]{return decoder.feed_async(frames[1],output(1));});
            const bool overlap=second.wait_for(std::chrono::milliseconds(250))==std::future_status::ready;
            if(!overlap)first.release();const bool submitted=second.get();
            require(overlap&&submitted&&decoder.outstanding()==2,"Parser blocked behind mapping or exceeded bound");
            auto third=std::async(std::launch::async,[&]{return decoder.feed_async(frames[2],output(2));});
            const bool bounded=third.wait_for(std::chrono::milliseconds(30))==std::future_status::timeout;
            first.release();require(third.get()&&bounded&&decoder.drain_async()&&observed==std::vector<unsigned>{0,1,2},"Bounded ordered decode failed");
            Gate changing;struct Release {Gate& gate;~Release(){gate.release();}} release{changing};
            auto old=output(3);auto prepare=std::move(old.prepare);
            old.prepare=[&changing,prepare=std::move(prepare)](uint64_t yuv,size_t pitch,uint32_t w,uint32_t h,HiveGpu* gpu){changing.block();prepare(yuv,pitch,w,h,gpu);};
            require(decoder.feed_async(frames[3],std::move(old))&&changing.await(),"Old format did not map");
            std::atomic<bool> correct_size=false;Decoder::AsyncOutput next;
            next.prepare=[&](uint64_t,size_t,uint32_t w,uint32_t h,HiveGpu*){correct_size=w==320&&h==240;};
            auto change=std::async(std::launch::async,[&]{return decoder.feed_async(resized,std::move(next));});
            const bool drained_old=change.wait_for(std::chrono::milliseconds(30))==std::future_status::timeout;
            changing.release();require(change.get()&&drained_old&&decoder.drain_async()&&correct_size,"Format change raced held surface");
        }
        phase="cancellation and expired admission";
        {
            Gate gate;std::atomic<bool> cancelled=false,admitted=true;
            Decoder decoder({},renderer.adapter(),true,[&]{return cancelled.load();});
            struct Cleanup {Gate& gate;Decoder& decoder;~Cleanup(){gate.release();decoder.stop_async();}} cleanup{gate,decoder};
            Decoder::AsyncOutput blocked;blocked.prepare=[&](uint64_t,size_t,uint32_t,uint32_t,HiveGpu*){gate.block();};
            require(decoder.feed_async(frames[0],std::move(blocked))&&gate.await(),"Cancellation worker did not start");
            require(decoder.feed_async(frames[1],{}),"Second reservation failed");
            Decoder::AsyncOutput last;last.admit=[&]{return admitted.load();};
            auto third=std::async(std::launch::async,[&]{return decoder.feed_async(frames[2],std::move(last));});
            const bool waited=third.wait_for(std::chrono::milliseconds(30))==std::future_status::timeout;
            admitted=false;gate.release();require(third.get()&&waited&&decoder.drain_async()&&decoder.outstanding()==0,"Admission was not rechecked after credit wait");
            cancelled=true;decoder.request_stop();decoder.stop_async();require(!decoder.feed_async(frames[2],{}),"Stopped decoder admitted work");
        }
        phase="latest ready GPU burst";
        {
            LatestPicture latest;std::array<Renderer::Picture,3> pixels;std::array<unsigned,3> sequence{};
            Decoder decoder({},renderer.adapter(),true);
            const auto submit=[&](unsigned n){auto reserved=std::make_shared<std::optional<size_t>>();Decoder::AsyncOutput output;
                output.prepare=[&,n,reserved](uint64_t yuv,size_t pitch,uint32_t w,uint32_t h,HiveGpu* gpu){
                    *reserved=latest.reserve();require(reserved->has_value(),"GPU burst exhausted ownership slots");
                    renderer.convert(pixels[**reserved],yuv,pitch,w,h,gpu);sequence[**reserved]=n;
                };
                output.complete=[&,reserved](uint32_t,uint32_t){require(latest.publish(**reserved).accepted,"GPU burst publication rejected");};
                return decoder.feed_async(frames[n],std::move(output));};
            require(submit(0)&&decoder.drain_async(),"Burst initial decode failed");
            const auto held=latest.take();require(held.has_value(),"Burst presenter could not retain initial image");
            for(unsigned i=1;i<frames.size();++i)require(submit(i),"Burst decoder stalled");
            require(decoder.drain_async(),"Burst mapper did not finish independently of display");
            const auto newest=latest.take(held);require(newest&&sequence[*newest]==7&&!latest.take(),"GPU burst became a playback FIFO");
            Image proof;renderer.draw_prepared(pixels[*newest],&proof,nullptr,nullptr,nullptr,{},true);
            const auto middle=(size_t(proof.height/2)*proof.width+proof.width/2)*4;
            require(!proof.bgra.empty()&&std::abs(int(proof.bgra[middle+2])-165)<=4,"Latest GPU image pixels were overwritten");
            unsigned attempts=0;renderer.set_present_for_test([&]{return ++attempts<3?DXGI_ERROR_WAS_STILL_DRAWING:S_OK;});
            require(renderer.present_when_ready([]{return true;})&&attempts==3,"Static final picture failed to retry");
            renderer.draw_prepared(pixels[*newest],nullptr,nullptr,nullptr,nullptr,{},true);const auto before=attempts;
            require(!renderer.present_when_ready([]{return false;})&&attempts==before,"Expired epoch pixels presented");
            // Keep the old presenting texture owned while a new resolution is
            // converted into another slot and enters the bounded CUDA cache.
            auto changed_slot=std::make_shared<std::optional<size_t>>();Decoder::AsyncOutput changed;
            changed.prepare=[&](uint64_t yuv,size_t pitch,uint32_t w,uint32_t h,HiveGpu* gpu){
                *changed_slot=latest.reserve();require(changed_slot->has_value(),"Resize exhausted ownership slots");
                renderer.convert(pixels[**changed_slot],yuv,pitch,w,h,gpu);sequence[**changed_slot]=8;
            };
            changed.complete=[&](uint32_t,uint32_t){require(latest.publish(**changed_slot).accepted,"Resized image publication failed");};
            require(decoder.feed_async(resized,std::move(changed))&&decoder.drain_async(),"Resized GPU conversion failed");
            const auto replacement=latest.take(newest);
            require(replacement&&pixels[*replacement].width==320&&pixels[*replacement].height==240,"Resized picture ownership failed");
            renderer.draw_prepared(pixels[*replacement],&proof,nullptr,nullptr,nullptr,{},true);
            require(std::abs(int(proof.bgra[middle+2])-190)<=4,"Resized registered texture retained old pixels");
            latest.release(*replacement);renderer.cancel_pending();
        }
        phase="CUDA interop / Present exclusion";
        {
            Renderer::Picture pixels;
            {Decoder initial([&](uint64_t yuv,size_t pitch,uint32_t w,uint32_t h,HiveGpu* gpu){renderer.convert(pixels,yuv,pitch,w,h,gpu);},renderer.adapter());initial.feed(frames[0]);}
            renderer.draw_prepared(pixels,nullptr,nullptr,nullptr,nullptr,{},true);
            Gate present;renderer.set_present_for_test([&]{present.block();return S_OK;});
            auto showing=std::async(std::launch::async,[&]{return renderer.retry_present();});
            const bool entered=present.await();std::atomic<bool> mapped=false;
            Decoder decoder({},renderer.adapter(),true,{},renderer.graphics_gate());Decoder::AsyncOutput output;
            output.prepare=[&](uint64_t yuv,size_t pitch,uint32_t w,uint32_t h,HiveGpu* gpu){renderer.convert(pixels,yuv,pitch,w,h,gpu);mapped=true;};
            const bool fed=decoder.feed_async(frames[0],std::move(output));Sleep(30);
            const bool excluded=!mapped;present.release();const bool shown=showing.get();
            require(entered&&fed&&excluded&&shown&&decoder.drain_async()&&mapped,"CUDA interop entered while Present owned the device");
            renderer.set_present_for_test({});renderer.cancel_pending();
        }
        phase="stop while decoder credits are occupied";
        {
            Gate gate;Decoder decoder({},renderer.adapter(),true);
            struct Cleanup {Gate& gate;Decoder& decoder;~Cleanup(){gate.release();decoder.stop_async();}} cleanup{gate,decoder};
            Decoder::AsyncOutput blocked;blocked.prepare=[&](uint64_t,size_t,uint32_t,uint32_t,HiveGpu*){gate.block();};
            require(decoder.feed_async(frames[0],std::move(blocked))&&gate.await(),"Stop test did not enter mapping");
            require(decoder.feed_async(frames[1],{}),"Stop test did not reserve second picture");
            auto third=std::async(std::launch::async,[&]{return decoder.feed_async(frames[2],{});});
            const bool waiting=third.wait_for(std::chrono::milliseconds(30))==std::future_status::timeout;
            decoder.request_stop();gate.release();
            const bool woke=third.wait_for(std::chrono::milliseconds(500))==std::future_status::ready;
            require(waiting&&woke&&!third.get(),"Stop did not cancel blocked credit admission");
            decoder.stop_async();require(decoder.outstanding()==0,"Stop retained decoded-picture reservations");
        }
        phase="worker error propagation";
        {
            Decoder decoder({},renderer.adapter(),true);Decoder::AsyncOutput broken;
            broken.prepare=[](uint64_t,size_t,uint32_t,uint32_t,HiveGpu*){throw std::runtime_error("deliberate output error");};
            bool caught=false;try{decoder.feed_async(frames[0],std::move(broken));decoder.drain_async();}
            catch(const std::exception& problem){caught=std::string(problem.what()).find("deliberate output error")!=std::string::npos;}
            require(caught,"Mapping failure stayed hidden without another packet");
        }
        phase="hidden live viewer startup and close";
        {
            Window live;
            live.value=CreateWindowExW(0,name,L"HiveDesk hidden live viewer",WS_POPUP,-32000,-32000,512,384,nullptr,nullptr,cls.hInstance,nullptr);
            require(live.value&&!IsWindowVisible(live.value),"Create independent hidden viewer window");
            MediaViewer viewer;std::string error;if(!viewer.open(live.value,error))throw std::runtime_error(error);
            auto frame=frames[0];uint64_t sequence=0,last_sent=0;
            auto source_tick=[&](bool initial=false){
                const auto now=now_us();const auto feedback=viewer.feedback();
                // Match the real host's bounded IDR recovery instead of making
                // a single cold-start frame immortal or weakening freshness.
                if(initial||(feedback.recover&&now-last_sent>=300000)){
                    frame.sequence=sequence++;frame.capture_time_us=now;frame.source_ready_us=0;frame.session=0;viewer.submit(frame);last_sent=now;
                }
            };source_tick(true);
            const auto deadline=Clock::now()+std::chrono::seconds(2);
            while(!viewer.has_frame()&&Clock::now()<deadline){MSG m{};while(PeekMessageW(&m,nullptr,0,0,PM_REMOVE)){TranslateMessage(&m);DispatchMessageW(&m);}source_tick();Sleep(2);}
            require(viewer.has_frame()&&!IsWindowVisible(live.value),"Hidden viewer failed prepared-pixel handshake");
            while(!viewer.has_presented_frame()&&Clock::now()<deadline){MSG m{};while(PeekMessageW(&m,nullptr,0,0,PM_REMOVE)){TranslateMessage(&m);DispatchMessageW(&m);}source_tick();Sleep(2);}
            require(viewer.has_presented_frame(),"First submitted-image gate depended on revealing the waiting cover");
            const auto began=Clock::now();viewer.close();require(Clock::now()-began<std::chrono::seconds(1),"Viewer shutdown failed to join workers");
            require(!viewer.has_presented_frame(),"Closed viewer retained an eligible first-frame signal");
        }
        report="PASS async NVDEC: bounded parser/map overlap, ordered colors, held-surface format drain, post-credit admission, cancellation/error propagation, latest-only GPU burst, static Present retry, epoch rejection and hidden live-view startup/close. No desktop capture/input or scanout claim.";return true;
    }catch(const std::exception& error){report="FAIL async NVDEC: "+phase+": "+error.what();return false;}
}
