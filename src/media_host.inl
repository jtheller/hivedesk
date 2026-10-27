// Included within namespace hive in media.cpp, after the private GPU classes.
struct MediaHost::Impl {
    std::atomic<bool> stop{true},keyframe{true};
    std::mutex wait_mutex;
    std::condition_variable changed;
    FrameTimer pacing_timer;
    std::thread worker;
    CaptureSequence sequence;
    StreamFeedback feedback{};
    uint64_t session=0,feedback_time=0,feedback_sequence=0,last_recovery=0;
    void wake(){changed.notify_all();pacing_timer.notify();}
};
MediaHost::MediaHost():impl_(std::make_unique<Impl>()){}
MediaHost::~MediaHost(){stop();}
bool MediaHost::start(const StreamSettings& settings,std::function<void(EncodedFrame)> output,std::string& error,
    std::function<bool()> ready_to_send,std::function<void(const StreamHealth&)> health,std::function<void(const SourceDiagnostics&)> diagnostics){
    stop();
    if(!output){error="Missing encoded-frame callback.";return false;}
    if(settings.session&&(settings.receiver_millihz<1000||settings.receiver_millihz>2000000)){error="Invalid receiver refresh rate.";return false;}
    impl_->sequence.begin(settings.session);
    {std::lock_guard lock(impl_->wait_mutex);impl_->stop=false;impl_->keyframe=true;impl_->feedback={};impl_->session=settings.session;impl_->feedback_time=impl_->feedback_sequence=impl_->last_recovery=0;}
    std::promise<std::string> initialized;auto ready=initialized.get_future();
    impl_->worker=std::thread([this,settings,output=std::move(output),ready_to_send=std::move(ready_to_send),health=std::move(health),diagnostics=std::move(diagnostics),initialized=std::move(initialized)]()mutable{
        bool announced=false;
        try{
            InputDesktop desktop;desktop.attach();Capture capture;
            const bool automatic=settings.session!=0;
            AdaptivePacer pacer(automatic?primary_refresh_millihz():120000,automatic?settings.receiver_millihz:120000);
            auto encoder_settings=settings;if(automatic)encoder_settings.receiver_millihz=pacer.target();
            Encoder encoder(capture.width,capture.height,encoder_settings,capture.adapter());
            AutoRateController rate_control(capture.width,capture.height,pacer.target());
            // Protected by wait_mutex. Completing GPU work never waits for the
            // pipe/socket: delivery is a third, independently bounded stage.
            SourceFrameQueue completed;
            EncoderCapacity encoder_capacity;
            uint64_t encoded_frames=0,send_wait=0;
            uint64_t encoded_bytes=0,window_qp_sum=0,window_qp_samples=0;
            uint64_t rate_frames=0,rate_bytes=0,rate_qp_sum=0,rate_qp_samples=0,sent_frames=0,total_send_wait=0;
            DiagnosticWindow<5> diagnostic_window;diagnostic_window.sample(now_us(),{});
            uint32_t live_encode=0,live_color=0,live_nvenc=0;uint64_t live_sample=0;
            std::exception_ptr failure;
            auto fail=[&](std::exception_ptr problem){
                {std::lock_guard lock(impl_->wait_mutex);if(!failure)failure=problem;impl_->stop=true;}
                impl_->wake();
            };
            auto request_refresh=[&]{impl_->keyframe=true;impl_->pacing_timer.notify();};
            auto downstream_ready=[&]{return !ready_to_send||ready_to_send();};
            std::thread drain,deliver;
            try{
                drain=std::thread([&]{try{
                    Encoder::Completion result;
                    while(encoder.receive(result)){
                        {
                            std::lock_guard lock(impl_->wait_mutex);
                            const auto now=result.completed_us;
                            live_encode=result.total_us;live_color=result.color_us;live_nvenc=result.nvenc_us;live_sample=now;
                            encoder_capacity.observe(now,result.submitted_us,result.color_us,result.frame.keyframe);
                            encoder_capacity.end_slot_wait();
                            ++encoded_frames;
                            encoded_bytes+=result.frame.data.size();
                            if(result.average_qp<=51){window_qp_sum+=result.average_qp;++window_qp_samples;}
                            // Intraframe recovery spikes are not a steady-state
                            // quality/bit-budget sample. Keep them in diagnostics.
                            if(!result.frame.keyframe){
                                ++rate_frames;rate_bytes+=result.frame.data.size();
                                if(result.average_qp<=51){rate_qp_sum+=result.average_qp;++rate_qp_samples;}
                            }
                            if(!impl_->stop){
                                result.frame.session=settings.session;
                                completed.push_reserved(std::move(result.frame),now_us(),result.total_us);
                                if(completed.needs_keyframe())request_refresh();
                            }else completed.cancel_completion();
                        }
                        impl_->changed.notify_all();
                    }
                }catch(...){fail(std::current_exception());}});
                deliver=std::thread([&]{try{
                    while(!impl_->stop){
                        std::optional<EncodedFrame> frame;
                        {
                            std::unique_lock lock(impl_->wait_mutex);
                            impl_->changed.wait(lock,[&]{return impl_->stop||!completed.empty();});
                            if(impl_->stop)break;
                            if(!downstream_ready()){
                                impl_->changed.wait_for(lock,std::chrono::milliseconds(20),[&]{return impl_->stop||downstream_ready();});
                                if(impl_->stop)break;
                                if(!downstream_ready())continue;
                            }
                            frame=completed.take(now_us());
                            if(completed.needs_keyframe())request_refresh();
                        }
                        impl_->changed.notify_all();
                        if(frame&&!impl_->stop){output(std::move(*frame));std::lock_guard lock(impl_->wait_mutex);++sent_frames;}
                    }
                }catch(...){fail(std::current_exception());}});
                log(std::string("Video pipeline: ")+(encoder.asynchronous()?"NVENC async events":"NVENC synchronous output worker")+", 4 IO slots / 2 in flight, bounded fresh-frame delivery.");
                initialized.set_value({});announced=true;
                bool have_picture=false;
                auto deadline=Clock::now(),last_keyframe=deadline;
                SendBackpressure send_pressure;
                uint64_t diagnostic_since=now_us(),last_health=0,last_report_stamp=0,network_floor=UINT64_MAX;
                uint64_t last_reconfigure=diagnostic_since;
                uint32_t capture_wait=0,network_extra=0;
                while(!impl_->stop){
                    const auto now=now_us();bool send_ready=false,slots_ready=false;
                    StreamFeedback report;uint64_t reported_at=0,sampled_at=0,report_sequence=0;uint32_t capacity=0;
                    AutoRateSample rate_sample;
                    {
                        std::lock_guard lock(impl_->wait_mutex);
                        report=impl_->feedback;reported_at=impl_->feedback_time;report_sequence=impl_->feedback_sequence;
                        sampled_at=encoder_capacity.sample_time(now);capacity=encoder_capacity.cost(now);
                        rate_sample.encoded_frames=rate_frames;rate_sample.encoded_bytes=rate_bytes;
                        rate_sample.qp_sum=rate_qp_sum;rate_sample.qp_samples=rate_qp_samples;
                        rate_sample.sent_frames=sent_frames;rate_sample.send_wait_us=total_send_wait;
                        send_ready=downstream_ready();
                        const bool recovery=completed.needs_keyframe()||impl_->keyframe.load();
                        slots_ready=completed.can_reserve()&&(!recovery||completed.reserved()==0);
                    }
                    if(automatic){
                        if(report.rate_details){
                            // The receiver observes completed-frame transport.
                            // With timed frames this excludes intrinsic encode
                            // work; the old roundtrip estimate did not.
                            network_extra=report.transit_extra_us;
                        }else if(report.capture_time_us&&report.capture_time_us!=last_report_stamp&&reported_at>=report.capture_time_us+report.arrival_age_us){
                            const auto transit=reported_at-report.capture_time_us-report.arrival_age_us;
                            network_floor=std::min(network_floor,transit);network_extra=uint32_t(std::min<uint64_t>(UINT32_MAX,transit-network_floor));last_report_stamp=report.capture_time_us;
                        }
                        const auto feedback_age=now>=reported_at?now-reported_at:0;
                        const bool fresh=reported_at&&feedback_age<1000000;
                        const bool bitrate_feedback=encoder.dynamic_rate()&&fresh&&report.rate_details;
                        if(encoder.dynamic_rate()){
                            rate_sample.receiver_report_id=bitrate_feedback?report_sequence:0;
                            rate_sample.received_bytes=report.received_bytes;rate_sample.received_frames=report.received_frames;
                            rate_sample.receive_sample_age_us=bitrate_feedback?uint32_t(std::min<uint64_t>(UINT32_MAX,uint64_t(report.receive_sample_age_us)+feedback_age)):UINT32_MAX;
                            rate_sample.receiver_queue_us=report.queue_us;rate_sample.pacing_millihz=pacer.rate();
                            rate_sample.recovering=report.recover;
                            pacer.set_external_limit(rate_control.observe(now,rate_sample).rate_limit_millihz);
                        }
                        const auto service=fresh&&(!report.rate_details||uint64_t(report.service_sample_age_us)+feedback_age<=1000000)?report.service_us:0;
                        const bool blocked=send_pressure.observe(now,send_ready);
                        // Modern CBR adjusts the bit budget first. The legacy
                        // pacing path remains available with older feedback or
                        // hardware that cannot reconfigure its bitrate.
                        pacer.observe(now,capacity,service,fresh?report.queue_us:0,blocked&&!bitrate_feedback,fresh&&report.recover,sampled_at,reported_at);
                        if(health&&now-last_health>=250000){health({settings.session,pacer.target(),pacer.rate(),pacer.pressure()});last_health=now;}
                        if(diagnostics&&now-diagnostic_since>=16000){
                            SourceDiagnostics d;d.session=settings.session;
                            {
                                std::lock_guard lock(impl_->wait_mutex);
                                const auto window=diagnostic_window.sample(now,{encoded_frames,encoded_bytes,send_wait,window_qp_sum,window_qp_samples});
                                d.encoded_millihz=window.rate(0,1000000000,4000000);d.encode_us=live_encode;
                                d.color_us=live_color;d.nvenc_us=live_nvenc;d.encode_details=true;
                                const auto age=now>=live_sample?now-live_sample:0;
                                d.encode_age_us=live_sample?uint32_t(std::min<uint64_t>(UINT32_MAX,age)):UINT32_MAX;
                                d.send_wait_us_per_second=window.rate(2,1000000,1000000);
                                d.actual_bitrate_bps=window.rate(1,8000000);
                                d.average_qp=window.values[4]?uint32_t(window.values[3]/window.values[4]):UINT32_MAX;
                                d.source_discarded=completed.discarded();
                            }
                            d.rate_details=true;d.automatic_cbr=encoder.automatic_rate();d.target_bitrate_bps=encoder.bitrate_bps();
                            d.quality_limited=encoder.dynamic_rate()&&rate_control.decision().rate_limit_millihz<pacer.target();
                            d.rate_reason=encoder.dynamic_rate()?uint32_t(rate_control.decision().reason):0;
                            d.receiver_service_us=service;d.receiver_queue_us=fresh?report.queue_us:0;d.receiver_recover=fresh&&report.recover;
                            d.feedback_sequence=report_sequence;d.sequence_gaps=report.sequence_gaps;d.age_expired=report.age_expired;d.queue_overflows=report.queue_overflows;
                            d.capture_wait_us=capture_wait;d.feedback_age_us=reported_at?uint32_t(std::min<uint64_t>(UINT32_MAX,feedback_age)):UINT32_MAX;d.network_extra_us=network_extra;
                            diagnostics(d);diagnostic_since=now;
                        }
                        if(encoder.dynamic_rate()&&now-last_reconfigure>=1000000){
                            const auto wanted=rate_control.decision();
                            const auto nominal=encoder.nominal_millihz(),paced=pacer.rate();
                            const auto difference=nominal>paced?nominal-paced:paced-nominal;
                            if(wanted.bitrate_bps!=encoder.bitrate_bps()||difference>=std::max(1000u,nominal/10)){
                                // Pause new submissions, drain at most the two
                                // accepted GPU jobs, then alter the existing RC
                                // state. Never insert EOS or force a recovery IDR.
                                if(encoder.in_flight()){
                                    std::unique_lock lock(impl_->wait_mutex);
                                    impl_->changed.wait_for(lock,std::chrono::milliseconds(20),[&]{return impl_->stop||encoder.in_flight()==0;});
                                    continue;
                                }
                                if(encoder.reconfigure(wanted.bitrate_bps,paced))last_reconfigure=now;
                            }
                        }
                    }
                    if(!send_ready){
                        std::unique_lock lock(impl_->wait_mutex);
                        const auto begin=now_us();
                        // Credit wait is network evidence only until credit
                        // returns. GPU slots/recovery may remain busy longer;
                        // their wait must not inflate the learned link budget.
                        impl_->changed.wait_for(lock,std::chrono::milliseconds(20),[&]{return impl_->stop||downstream_ready();});
                        const auto blocked=now_us()-begin;send_wait+=blocked;total_send_wait+=blocked;
                        continue;
                    }
                    if(!slots_ready||!encoder.ready()){
                        std::unique_lock lock(impl_->wait_mutex);
                        // Only GPU-slot starvation across a scheduled capture
                        // deadline demonstrates insufficient encoder throughput.
                        // Output/network credit and deliberate recovery drains
                        // are different constraints, as is waiting before a tick.
                        if(!encoder.ready()&&downstream_ready()&&!completed.needs_keyframe()&&!impl_->keyframe.load())
                            encoder_capacity.begin_slot_wait(uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(deadline.time_since_epoch()).count()));
                        impl_->changed.wait_for(lock,std::chrono::milliseconds(20),[&]{return impl_->stop||(downstream_ready()&&completed.can_reserve()&&encoder.ready()&&(!(completed.needs_keyframe()||impl_->keyframe.load())||completed.reserved()==0));});
                        encoder_capacity.end_slot_wait();
                        continue;
                    }
                    const auto interval=automatic?std::chrono::nanoseconds(1000000000000ull/pacer.rate()):settings.fps?std::chrono::nanoseconds(1000000000ull/settings.fps):std::chrono::nanoseconds(0);
                    while(interval.count()&&Clock::now()<deadline&&!impl_->stop&&!impl_->keyframe)impl_->pacing_timer.wait_until(deadline);
                    if(impl_->stop)break;
                    // Recovery starts with an idle encoder. It cannot enqueue
                    // IDRs behind obsolete jobs indefinitely during overload.
                    if(impl_->keyframe&&encoder.in_flight()!=0)continue;
                    const auto capture_started=Clock::now();const auto capture_begin=now_us();
                    const bool changed=capture.next();capture_wait=uint32_t(std::min<uint64_t>(10000000,now_us()-capture_begin));have_picture=have_picture||changed;
                    // Sequence numbers survive capture restarts; encoder state
                    // does not. Keep forcing the first IDR through capture
                    // timeouts and unavailable slots until a submit succeeds.
                    const bool forced=impl_->keyframe.exchange(false)||impl_->sequence.first()||(!automatic&&Clock::now()-last_keyframe>=std::chrono::seconds(1));
                    if(!changed&&(!forced||!have_picture))continue;
                    // No raw-frame FIFO. When slots/credit recover, acquire the
                    // current desktop instead of consuming old captured images.
                    {
                        std::lock_guard lock(impl_->wait_mutex);
                        if(impl_->stop)break;
                        if((forced&&completed.reserved()!=0)||!completed.reserve_completion()){if(forced)impl_->keyframe=true;continue;}
                    }
                    bool submitted=false,completion_queued=false;
                    const auto sequence=impl_->sequence.next();
                    try{submitted=encoder.submit_texture(capture.texture(),sequence,forced||sequence==0,now_us(),&completion_queued);}
                    catch(...){if(completion_queued)impl_->sequence.submitted();else{std::lock_guard lock(impl_->wait_mutex);completed.cancel_completion();}throw;}
                    if(submitted){
                        impl_->sequence.submitted();if(forced)last_keyframe=Clock::now();deadline=advance_frame_deadline(deadline,capture_started,interval);
                    }else{
                        std::lock_guard lock(impl_->wait_mutex);completed.cancel_completion();if(forced)impl_->keyframe=true;
                    }
                }
            }catch(...){fail(std::current_exception());}
            // Every exit joins both consumers before encoder/capture destruction.
            // Accepted GPU jobs drain even after stop; their results are discarded.
            {std::lock_guard lock(impl_->wait_mutex);impl_->stop=true;}
            impl_->wake();
            try{encoder.finish();}catch(...){fail(std::current_exception());}
            if(drain.joinable())drain.join();if(deliver.joinable())deliver.join();
            if(failure)std::rethrow_exception(failure);
        }catch(const std::exception& problem){
            if(!announced)initialized.set_value(problem.what());
            else{const std::string message=std::string("Media host stopped: ")+problem.what();log(message);OutputDebugStringA(message.c_str());}
            impl_->stop=true;
        }catch(...){
            if(!announced)initialized.set_value("Unknown media pipeline failure.");
            else log("Media host stopped: unknown pipeline callback failure.");
            impl_->stop=true;
        }
    });
    error=ready.get();if(!error.empty()){stop();return false;}return true;
}
void MediaHost::stop(){{std::lock_guard lock(impl_->wait_mutex);impl_->stop=true;}impl_->wake();if(impl_->worker.joinable())impl_->worker.join();}
bool MediaHost::running()const{return !impl_->stop.load();}
void MediaHost::request_keyframe(){{std::lock_guard lock(impl_->wait_mutex);impl_->keyframe=true;}impl_->wake();}
void MediaHost::notify_ready(){std::lock_guard lock(impl_->wait_mutex);impl_->changed.notify_all();}
void MediaHost::feedback(const StreamFeedback& feedback){
    std::lock_guard lock(impl_->wait_mutex);if(!feedback.session||feedback.session!=impl_->session||impl_->stop)return;
    // Extended peers identify snapshots; duplicate/stale feedback must not be
    // made fresh by receipt or repeatedly trigger recovery/bit-budget changes.
    if(feedback.rate_details&&impl_->feedback.rate_details&&feedback.feedback_sequence<=impl_->feedback.feedback_sequence)return;
    impl_->feedback=feedback;impl_->feedback_time=now_us();++impl_->feedback_sequence;
    if(feedback.recover&&impl_->feedback_time-impl_->last_recovery>=300000){impl_->last_recovery=impl_->feedback_time;impl_->keyframe=true;impl_->wake();}
}
