#include <winsock2.h>
#include "../src/datagram_window.h"
#include "../src/datagram_video.h"
#include "../src/datagram_recovery.h"
#include <cstdio>
#include <atomic>
#include <chrono>
#include <thread>
#include <stdexcept>

using hive::datagram::Window;using hive::datagram::Bytes;
void require(bool ok,const char* text){if(!ok)throw std::runtime_error(text);}
Bytes image(size_t n,uint8_t marker){Bytes b(n);for(size_t i=0;i<n;++i)b[i]=uint8_t(i*17+marker);return b;}
std::vector<Bytes> drain(Window& w,uint64_t now){std::vector<Bytes> out;for(unsigned i=0;i<1000;++i){auto p=w.poll(now);if(!p)break;out.push_back(std::move(*p));}return out;}
void reference_tests(){
    Window a,b;auto first=image(3000,7),second=image(800,8);
    require(!a.enqueue(first,false,1,1000),"P without an IDR accepted");
    require(a.enqueue(first,true,1,1000),"IDR rejected");auto packets=drain(a,10000);require(packets.size()==3,"fragmentation");
    require(a.ready()&&a.enqueue(second,false,1,10001),"whole frame ACK stop-and-wait");
    auto next=drain(a,20000);require(next.size()==1,"P fragmented incorrectly");
    require(b.receive(next[0],30000)&&b.take_pictures().empty(),"dependent picture escaped before its IDR");
    require(b.receive(packets[2],30001)&&b.receive(packets[0],30002)&&b.receive(packets[0],30003),"reorder/duplicate rejected");
    require(b.take_pictures().empty(),"incomplete IDR exposed");
    require(b.receive(packets[1],31000),"last fragment rejected");auto output=b.take_pictures();
    require(output.size()==2&&output[0].bytes==first&&output[1].bytes==second,"ordered exact reassembly");
    require(output[0].started==30001&&output[0].completed==31000,"local assembly timing");
    require(output[1].started==30000&&output[1].completed==30000,"reference wait restamped an old picture as newly received");
    for(auto& ack:drain(b,32000))require(a.receive(ack,40000),"ACK rejected");
    require(a.stats().flight==0&&a.stats().owned==0,"ACK storage not freed");
    const auto cwnd=a.stats().cwnd;Bytes forged{Window::Ack,1};hive::datagram::put(forged,1000000,8);
    require(a.receive(forged,41000)&&a.stats().cwnd==cwnd,"unsent ACK grew window");
    require(a.enqueue(image(2200,9),false,1,42000),"next P rejected");auto old=drain(a,50000);
    a.cancel(2);require(a.stats().owned==2200,"cancel wrong session");a.cancel(1);
    require(a.stats().owned==0&&!a.enqueue(second,false,1,51000),"cancel left predictive output live");
    require(a.enqueue(second,true,2,52000),"new session IDR");auto fresh=drain(a,60000);
    require(b.receive(fresh[0],70000),"new epoch");output=b.take_pictures();require(output.size()==1&&output[0].bytes==second,"new epoch not delivered");
    for(auto& p:old)require(b.receive(p,71000),"old epoch rejected connection");
    require(b.take_pictures().empty(),"old session resurrected");
    for(size_t n=0;n<33;++n)require(!b.receive(Bytes(packets[0].begin(),packets[0].begin()+n),80000),"truncated metadata accepted");
    auto malformed=packets[0];for(size_t i=25;i<29;++i)malformed[i]=255;
    require(!b.receive(malformed,80000),"unbounded frame allocation accepted");
    Window timeout;require(timeout.enqueue(first,true,1,1),"timeout setup");drain(timeout,1000);drain(timeout,Window::Deadline+2);
    require(timeout.stats().owned==0&&timeout.take_key_request(),"bounded lifetime failed");
    Window switching;
    for(uint64_t now=1;now<100000;now+=100){if(switching.ready())require(switching.enqueue(image(1000,1),true,now,now),"rapid session enqueue");drain(switching,now);}
    require(switching.stats().sent<=32&&switching.stats().flight<=32,"IDR/session churn bypassed congestion credits");
    Window stalled_receiver;for(auto& p:packets)require(stalled_receiver.receive(p,1),"receive expiration setup");
    drain(stalled_receiver,Window::Deadline+2);
    require(stalled_receiver.receive(next[0],Window::Deadline+3)&&stalled_receiver.take_pictures().empty(),"expired retained references exposed dependent pictures");
    std::puts("PASS reorder, missing reference, duplicate, session/epoch fence, bounds, ACK ownership and deadline");
}
void recovery_policy_tests(){
    hive::datagram::Recovery r;r.fallback(100);
    require(!r.probe(5000099,false),"recovery ignored cooldown");
    auto id=r.probe(5000100,false);require(id&&!r.reply(id+1,5000200),"unsolicited probe reply accepted");
    require(r.reply(id,5000200)&&!r.reply(id,5000201)&&!r.ready(5000200),"one/duplicate reply promoted path");
    id=r.probe(6000100,false);require(r.reply(id,6000200)&&!r.ready(6000200),"two replies promoted path");
    id=r.probe(7000100,false);require(r.reply(id,7000200)&&r.ready(7000200),"stable path never became eligible");
    require(!r.ready(9000200),"old evidence promoted path");
    id=r.probe(10000000,false);require(r.reply(id,10000100)&&!r.ready(10000100),"scheduler stall retained old healthy streak");
    r.fallback(10000000);require(!r.probe(19999999,false),"repeated failure did not back off");
    id=r.probe(20000000,false);require(id&&!r.reply(id,22000000),"timed-out reply accepted");
    id=r.probe(22000000,false);require(id&&r.reply(id,22000100)&&!r.ready(22000100),"timeout retained successful streak");
    r.stable();r.fallback(30000000);require(r.probe(35000000,false)!=0,"healthy period did not restore normal cooldown");
    Window a,b;auto packet=image(800,1);
    require(a.enqueue(packet,true,1,1),"expiry setup");const auto old=drain(a,1000);for(auto& p:old)b.receive(p,1000);
    require(b.enqueue(packet,true,2,1900000),"reverse expiry setup");drain(b,1901000);drain(b,2001001);
    auto s=b.stats();require(s.owned&&s.receive_expired==1&&s.send_expired==0,"receiver expiry contaminated sender failure");
    drain(b,3901000);s=b.stats();require(s.send_expired==1&&s.receive_expired==1&&s.expired==2,"directional expiry accounting");
    // The TCP barrier reserves a newer reference epoch, even without a frame.
    const auto epoch=a.resume_sender();b.resume_receiver(epoch);
    for(auto& p:old)b.receive(p,4000000);require(b.take_pictures().empty(),"late pre-barrier UDP escaped fence");
    require(a.enqueue(packet,true,3,4000000),"resume IDR rejected");auto fresh=drain(a,4001000);
    for(auto& p:fresh)b.receive(p,4002000);
    require(b.take_pictures().size()==1,"barrier did not admit fresh IDR");
    require(a.resume_sender()>epoch,"aborted attempt reused a recovery epoch");
    std::puts("PASS directional expiry, recovery evidence/cooldown/backoff and reference boundaries");
}
void rtt_change_test(){
    Window a,b;uint64_t now=1;bool first=true;
    const auto sample=[&](uint64_t rtt){
        require(a.enqueue(image(100,1),first,1,now),"RTT sample enqueue");first=false;
        auto p=drain(a,now+1000);require(p.size()==1,"RTT sample send");
        b.receive(p[0],now+1000+rtt/2);b.take_pictures();
        for(auto& ack:drain(b,now+2000+rtt/2))a.receive(ack,now+1000+rtt);
        now+=200000;
    };
    for(unsigned i=0;i<10;++i)sample(4000);
    require(a.stats().min_rtt==4000,"initial RTT floor");
    for(unsigned i=0;i<20;++i)sample(140000);
    require(a.stats().min_rtt==4000,"brief congestion reset RTT floor");
    for(unsigned i=0;i<40;++i)sample(140000);
    require(a.stats().min_rtt==140000,"persistent changed path retained lifetime RTT floor");
    const auto before=a.stats().cwnd;for(unsigned i=0;i<10;++i)sample(140000);
    require(a.stats().cwnd>=before,"new normal RTT still triggered congestion");
    sample(4000);require(a.stats().min_rtt==4000,"lower latency path was not learned immediately");
    now+=20000000;sample(180000);require(a.stats().min_rtt==180000,"idle resume reused expired floor");
    std::puts("PASS transient queue pressure retained, persistent/idle RTT changes age, lower RTT immediate");
}
struct Delivery {bool to_b;Bytes packet;};
void simulated_path(uint64_t one_way,unsigned loss_every,bool changed=false){
    Window a,b;std::multimap<uint64_t,Delivery> network;uint64_t link_a=0,link_b=0,packets=0,next_frame=1,frames=0,delivered=0,max_gap=0,last_picture=0,first_picture=0;
    uint64_t late_delivered=0;
    const auto enqueue=[&](bool to_b,Bytes packet,uint64_t now){
        const auto number=++packets;
        if(loss_every&&number%loss_every==0)return;
        auto& link=to_b?link_a:link_b;
        // 20 Mbps each direction, finite 50 ms queue, deterministic reorder.
        if(link>now+50000)return;
        link=std::max(link,now)+(packet.size()+28)*8*1000000/20000000;
        const auto jitter=(number%29==0)?3000:0;
        const auto latency=changed&&now>=3000000?70000:one_way;
        network.emplace(link+latency+jitter,Delivery{to_b,std::move(packet)});
    };
    for(uint64_t now=1;now<(changed?45000000u:10000000u);now+=250){
        while(!network.empty()&&network.begin()->first<=now){auto item=std::move(network.begin()->second);network.erase(network.begin());require((item.to_b?b:a).receive(item.packet,now),"simulation wire rejected");}
        bool key=frames==0||a.take_key_request();
        if(a.ready()&&(key||now>=next_frame)){
            // Same reference pattern as 60 Hz video: a large initial IDR and
            // modest P-frames. Content marker proves no corruption/reordering.
            require(a.enqueue(image(key?780000:16000,uint8_t(frames)),key,1,now),"simulation enqueue");++frames;next_frame=now+16667;
        }
        for(auto& p:drain(a,now))enqueue(true,std::move(p),now);
        for(auto& p:drain(b,now))enqueue(false,std::move(p),now);
        for(auto& picture:b.take_pictures()){
            require(picture.bytes==image(picture.bytes.size(),picture.bytes[0]),"simulation payload corruption");
            if(!first_picture)first_picture=now;if(last_picture)max_gap=std::max(max_gap,now-last_picture);last_picture=now;++delivered;
            if(now>=40000000)++late_delivered;
        }
    }
    auto s=a.stats();std::printf("SIM RTT=%llu ms loss_every=%u sent_frames=%llu delivered=%llu first_ms=%llu max_gap_ms=%llu RTX=%llu cwnd=%.1f\n",one_way/500,loss_every,frames,delivered,first_picture/1000,max_gap/1000,s.retransmitted,s.cwnd);
    require(first_picture&&first_picture<1500000,"first picture took over 1.5 s on 20 Mbps path");
    require(delivered>(loss_every?80u:400u),"sustained path throughput collapsed");
    require((changed||max_gap<1500000)&&s.owned<Window::MaxBytes&&s.flight<=Window::MaxFlight,"unbounded backlog or stall");
    if(changed){std::printf("CHANGED RTT=2 -> 140 ms final_5s_frames=%llu floor_us=%llu expired=%llu\n",late_delivered,s.min_rtt,s.send_expired);require(late_delivered>240&&s.min_rtt>=140000,"persistent RTT increase never recovered video throughput");}
}
void recovery_timeout_test(){
    using hive::DatagramVideo;WSADATA ws{};require(WSAStartup(MAKEWORD(2,2),&ws)==0,"Winsock recovery");
    struct Cleanup{~Cleanup(){WSACleanup();}} cleanup;
    std::array<uint8_t,32> key{};key[0]=19;auto bytes=std::make_shared<std::atomic<uint64_t>>(0);
    std::atomic<uint64_t> barrier=0;std::atomic<unsigned> reverse=0;
    DatagramVideo::Hooks ha;ha.resume=[&](uint64_t epoch){barrier=epoch;return true;};ha.picture=[&](Bytes,uint64_t,uint64_t){++reverse;};
    DatagramVideo a("127.0.0.1",key,true,bytes,ha,"127.0.0.1",true),b("127.0.0.1",key,false,bytes,{},"127.0.0.1",true);
    a.offer(b.port());b.offer(a.port());a.start();b.start();
    auto until=std::chrono::steady_clock::now()+std::chrono::seconds(3);
    while((!a.ready()||!b.ready())&&std::chrono::steady_clock::now()<until)std::this_thread::sleep_for(std::chrono::milliseconds(1));
    b.tcp_received();require(a.send(image(1000,1),true,1),"recovery timeout setup");
    until=std::chrono::steady_clock::now()+std::chrono::seconds(13);
    while(!barrier&&std::chrono::steady_clock::now()<until)std::this_thread::sleep_for(std::chrono::milliseconds(1));
    require(barrier&&a.route()==DatagramVideo::Route::Recovering,"recovery barrier not requested");
    require(!a.ready()&&!a.accept_tcp(true),"media escaped in-progress barrier");
    until=std::chrono::steady_clock::now()+std::chrono::seconds(4);
    while(a.route()==DatagramVideo::Route::Recovering&&std::chrono::steady_clock::now()<until)std::this_thread::sleep_for(std::chrono::milliseconds(1));
    require(a.route()==DatagramVideo::Route::Tcp&&!a.accept_tcp(false)&&a.accept_tcp(true),"unacknowledged barrier did not restore TCP with IDR");
    require(b.resume_received(barrier)&&!b.resume_received(barrier),"late/duplicate barrier boundary");
    require(b.ready()&&b.send(image(1000,2),true,2),"barrier timeout disabled reciprocal lane");
    until=std::chrono::steady_clock::now()+std::chrono::seconds(1);
    while(!reverse&&std::chrono::steady_clock::now()<until)std::this_thread::sleep_for(std::chrono::milliseconds(1));
    require(reverse==1&&a.route()==DatagramVideo::Route::Tcp,"late barrier resurrected UDP or reciprocal delivery failed");
    a.stop();b.stop();a.join();b.join();
    std::puts("PASS recovery barrier timeout, TCP fresh-reference fallback, late marker and reciprocal isolation");
}
void socket_test(){
    using hive::DatagramVideo;WSADATA ws{};require(WSAStartup(MAKEWORD(2,2),&ws)==0,"Winsock");
    struct Cleanup{~Cleanup(){WSACleanup();}} cleanup;
    std::array<uint8_t,32> key{};for(size_t i=0;i<key.size();++i)key[i]=uint8_t(i+3);
    std::atomic<unsigned> rx_a=0,rx_b=0;std::atomic<bool> damaged=false,consumer_ready=true;
    const auto content=image(64000,71);auto progress_a=std::make_shared<std::atomic<uint64_t>>(0),progress_b=std::make_shared<std::atomic<uint64_t>>(0);
    DatagramVideo::Hooks ha,hb;
    ha.picture=[&](Bytes b,uint64_t start,uint64_t end){if(b!=content||!start||end<start)damaged=true;++rx_a;};
    hb.picture=[&](Bytes b,uint64_t start,uint64_t end){if(b!=content||!start||end<start)damaged=true;++rx_b;};
    hb.receive_ready=[&](const Bytes&){return consumer_ready.load();};
    // Distinct local addresses reproduce multi-address hosts: wildcard binding
    // would select 127.0.0.1 and fail the opposite peer's strict endpoint check.
    DatagramVideo a("127.0.0.3",key,true,progress_a,ha,"127.0.0.2"),b("127.0.0.2",key,false,progress_b,hb,"127.0.0.3");
    require(a.offer(b.port())&&b.offer(a.port()),"port exchange");a.start();b.start();
    auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(8);unsigned sent_a=0,sent_b=0;
    while(std::chrono::steady_clock::now()<deadline&&(rx_a<120||rx_b<120)){
        if(sent_a<120&&a.ready()){require(a.send(content,sent_a==0,7),"socket send a");++sent_a;}
        if(sent_b<120&&b.ready()){require(b.send(content,sent_b==0,9),"socket send b");++sent_b;}
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    std::printf("SOCKET a=%u b=%u%s%s\n",rx_a.load(),rx_b.load(),a.diagnostics().c_str(),b.diagnostics().c_str());
    require(!damaged&&rx_a==120&&rx_b==120,"full duplex socket transfer");
    require(progress_a->load()==120*content.size()&&progress_b->load()==120*content.size(),"unique authenticated byte feedback");
    consumer_ready=false;unsigned burst=0;deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
    while(burst<10&&std::chrono::steady_clock::now()<deadline){if(a.ready()){require(a.send(content,false,7),"burst send");++burst;}std::this_thread::sleep_for(std::chrono::milliseconds(1));}
    while(progress_b->load()<130*content.size()&&std::chrono::steady_clock::now()<deadline)std::this_thread::sleep_for(std::chrono::milliseconds(1));
    require(burst==10&&progress_b->load()==130*content.size()&&rx_b==120&&a.route()==DatagramVideo::Route::Datagram,"decoder credit blocked packet ACK/progress or leaked a picture");
    consumer_ready=true;deadline=std::chrono::steady_clock::now()+std::chrono::seconds(1);
    while(rx_b<130&&std::chrono::steady_clock::now()<deadline)std::this_thread::sleep_for(std::chrono::milliseconds(1));
    require(rx_b==130,"decoder credit did not resume retained references");
    // Stall B's consumer beyond its receive deadline while B continues sending
    // to A. Receive expiry must not switch B's healthy sender to TCP.
    consumer_ready=false;require(a.send(content,true,7),"duplex expiry setup");
    deadline=std::chrono::steady_clock::now()+std::chrono::milliseconds(2300);unsigned reverse_sent=0;
    while(std::chrono::steady_clock::now()<deadline){
        if(b.ready()){require(b.send(content,false,9),"reverse send during receiver stall");++reverse_sent;}
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    hive::VideoSendDiagnostics duplex;b.diagnostics(duplex);
    require(reverse_sent>100&&duplex.packet_receive_expired==1&&duplex.packet_send_expired==0&&b.route()==DatagramVideo::Route::Datagram,"receiver stall downgraded healthy reciprocal UDP");
    consumer_ready=true;require(a.send(content,true,7),"post-expiry fresh IDR");
    deadline=std::chrono::steady_clock::now()+std::chrono::seconds(1);
    while(rx_b<131&&std::chrono::steady_clock::now()<deadline)std::this_thread::sleep_for(std::chrono::milliseconds(1));
    require(rx_b==131,"post-expiry receiver did not recover");
    b.tcp_received();require(a.ready()&&a.send(content,true,10),"late UDP setup");
    std::this_thread::sleep_for(std::chrono::milliseconds(80));require(rx_b==131,"UDP callback escaped TCP fallback fence");
    deadline=std::chrono::steady_clock::now()+std::chrono::seconds(3);
    while(a.route()!=DatagramVideo::Route::Tcp&&std::chrono::steady_clock::now()<deadline)std::this_thread::sleep_for(std::chrono::milliseconds(10));
    require(a.route()==DatagramVideo::Route::Tcp&&!a.accept_tcp(false)&&a.accept_tcp(true),"midstream black hole did not recover through a fresh TCP reference");
    const auto before_reverse=rx_a.load();
    require(b.ready()&&b.send(content,true,11),"reciprocal lane disabled by fallback");
    deadline=std::chrono::steady_clock::now()+std::chrono::seconds(1);
    while(rx_a==before_reverse&&std::chrono::steady_clock::now()<deadline)std::this_thread::sleep_for(std::chrono::milliseconds(1));
    require(rx_a>before_reverse,"reciprocal UDP lost after other direction fell back");
    const auto stopped=std::chrono::steady_clock::now();
    a.stop();b.stop();a.join();b.join();
    require(std::chrono::steady_clock::now()-stopped<std::chrono::milliseconds(250),"UDP shutdown exceeded bounded wake");
    DatagramVideo fallback("127.0.0.1",key,true,progress_a,{});require(fallback.offer(0),"fallback offer");fallback.start();
    require(fallback.route()==DatagramVideo::Route::Tcp&&!fallback.accept_tcp(false)&&fallback.accept_tcp(true),"fallback failed to require IDR");fallback.stop();fallback.join();
    DatagramVideo unavailable("127.0.0.1",key,true,progress_a,{});unavailable.offer(unavailable.port());unavailable.start();
    deadline=std::chrono::steady_clock::now()+std::chrono::seconds(4);
    while(unavailable.route()!=DatagramVideo::Route::Tcp&&std::chrono::steady_clock::now()<deadline)std::this_thread::sleep_for(std::chrono::milliseconds(10));
    require(unavailable.route()==DatagramVideo::Route::Tcp,"negotiation never fell back");unavailable.stop();unavailable.join();
    std::puts("PASS encrypted UDP duplex, progress, TCP fencing, unavailable path and bounded stop");
}
int main(){try{reference_tests();recovery_policy_tests();rtt_change_test();simulated_path(1000,0);simulated_path(70000,0);simulated_path(70000,100);simulated_path(1000,0,true);std::string crypto;require(hive::DatagramVideo::self_test(crypto),crypto.c_str());std::puts(crypto.c_str());socket_test();recovery_timeout_test();return 0;}catch(const std::exception& e){std::printf("FAIL %s\n",e.what());return 1;}}
