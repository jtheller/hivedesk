#include "hivedesk/transport.h"
#include "hivedesk/media.h"
#include "hivedesk/config.h"
#include <atomic>
#include <algorithm>
#include <future>
#include <cstdio>
#include <cstdlib>
#include <thread>
void pump(){MSG m{};while(PeekMessageW(&m,nullptr,0,0,PM_REMOVE)){TranslateMessage(&m);DispatchMessageW(&m);}Sleep(2);}
LRESULT CALLBACK proc(HWND h,UINT m,WPARAM w,LPARAM l){if(m==WM_NCHITTEST)return HTTRANSPARENT;if(m==WM_MOUSEACTIVATE)return MA_NOACTIVATE;return DefWindowProcW(h,m,w,l);}
int main(int argc,char** argv){
 setvbuf(stdout,nullptr,_IONBF,0);SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
 const char* address=argc>1?argv[1]:"127.0.0.1";
 auto config=hive::load_config();hive::Transport client;auto viewer=std::make_shared<hive::MediaViewer>();
 const uint64_t session=GetTickCount64(),began=session;std::atomic<bool> declined=false;std::atomic<unsigned> frames=0;
 hive::TransportCallbacks cb;
 cb.frame_credit=[&](const std::string&,uint64_t id){return id!=session||viewer->receive_ready();};
 cb.status=[](const std::string& text){std::printf("transport %s\n",text.c_str());};
 cb.peer=[](const hive::Peer& p){if(!p.version.empty())std::printf("peer=%s version=%s\n",p.name.c_str(),p.version.c_str());};
 cb.health=[](const std::string&,const hive::StreamHealth& h){static uint32_t target=0,cap=0,pressure=UINT32_MAX;if(target==h.target_millihz&&cap==h.rate_millihz&&pressure==h.pressure)return;target=h.target_millihz;cap=h.rate_millihz;pressure=h.pressure;std::printf("health target_millihz=%u cap_millihz=%u pressure=%u\n",target,cap,pressure);};
 cb.frame=[&](const std::string&,hive::EncodedFrame f){if(f.session!=session)return;const auto n=++frames;if(n<=12||n%30==0)std::printf("frame t=%llu n=%u seq=%llu bytes=%zu source_us=%llu key=%d assembly_us=%llu\n",GetTickCount64()-began,n,f.sequence,f.data.size(),f.source_ready_us-f.capture_time_us,f.keyframe,f.receive_completed_us>=f.receive_started_us?f.receive_completed_us-f.receive_started_us:0);viewer->submit(std::move(f));};
 cb.diagnostics=[](const std::string&,const hive::SourceDiagnostics& d){static uint64_t last=0;const auto tick=GetTickCount64();if(tick-last<1000)return;last=tick;std::printf("source fps=%u encode_us=%u capture_wait_us=%u wait_us=%u feedback_age=%u network_extra=%u bitrate=%u actual=%u qp=%u recover=%d reason=%u discarded=%llu\n",d.encoded_millihz,d.encode_us,d.capture_wait_us,d.send_wait_us_per_second,d.feedback_age_us,d.network_extra_us,d.target_bitrate_bps,d.actual_bitrate_bps,d.average_qp,d.receiver_recover,d.rate_reason,d.source_discarded);};
 cb.host_ended=[&](const std::string&,uint64_t id){if(id==session)declined=true;};
 std::string error,peer;
 if(!client.start({"readonly-video-"+std::to_string(GetCurrentProcessId()),"Read-only render diagnostic",config.group_key,0,false,true},cb,error)||!client.connect(address,config.port,peer,error)){std::printf("FAIL connect %s\n",error.c_str());return 1;}
 std::printf("CONNECTED t=%llu\n",GetTickCount64()-began);
 WNDCLASSW c{};c.lpfnWndProc=proc;c.hInstance=GetModuleHandleW(nullptr);c.lpszClassName=L"HiveDesk.ReadonlyRemote";RegisterClassW(&c);
 HWND window=CreateWindowExW(WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE|WS_EX_NOREDIRECTIONBITMAP|WS_EX_TRANSPARENT,c.lpszClassName,L"Read-only render diagnostic",WS_POPUP,GetSystemMetrics(SM_CXSCREEN)-340,GetSystemMetrics(SM_CYSCREEN)-220,320,180,nullptr,nullptr,c.hInstance,nullptr);
 SetPropW(window,L"HiveDesk.PeekSurface",reinterpret_cast<HANDLE>(1));
 const bool force_software=argc>3&&std::string(argv[3])=="--software";
 auto opening=std::async(std::launch::async,[&]{return viewer->open(window,error,{},true,{},force_software);});while(opening.wait_for(std::chrono::milliseconds(0))!=std::future_status::ready)pump();
 if(!opening.get()){std::printf("FAIL open %s\n",error.c_str());client.stop();DestroyWindow(window);return 1;}
 hive::StreamSettings settings;settings.session=session;settings.receiver_millihz=60000;settings.warm=true;
 std::printf("REQUEST t=%llu session=%llu\n",GetTickCount64()-began,session);
 client.request_stream(peer,settings);client.preview(peer,session,true);viewer->preview(true);
 uint64_t feedback_at=0,renewed=GetTickCount64(),reported=0;bool shown=false;uint64_t shown_sequence=UINT64_MAX;unsigned presented_pictures=0;
 const auto duration=argc>2?std::strtoul(argv[2],nullptr,10):45000;
 const auto until=GetTickCount64()+std::clamp<unsigned long>(duration,1000,120000);
 while(!declined&&GetTickCount64()<until){pump();const auto now=GetTickCount64();
  if(now-feedback_at>=250){auto f=viewer->feedback();if(!f.session)f.session=session;if(f.session)client.send_feedback(peer,f);feedback_at=now;}
  if(now-renewed>=2000){client.request_stream(peer,settings);renewed=now;}
  if(!shown&&viewer->has_presented_frame()){SetWindowPos(window,HWND_TOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE|SWP_SHOWWINDOW);viewer->peek_opacity(true);shown=true;}
  const auto displayed=viewer->cursor_presentation();if(displayed.valid&&displayed.sequence!=shown_sequence){shown_sequence=displayed.sequence;++presented_pictures;std::printf("PRESENT seq=%llu count=%u t=%llu\n",shown_sequence,presented_pictures,now-began);}
  if(now-reported>=1000){const auto d=viewer->diagnostics();std::printf("cost decode_us=%u render_us=%u convert_us=%u draw_us=%u present_us=%u service_p90_us=%u queue_us=%u\n",d.decode_us,d.render_us,d.convert_us,d.draw_us,d.present_us,d.service_p90_us,d.queue_us);std::printf("t=%llu frames=%u prepared=%d presented=%d rx=%.1f decode=%.1f present=%.1f age=%llu gaps=%llu overflow=%llu transit_us=%u status=%s\ntcp:%s\n",now-began,frames.load(),viewer->has_frame(),viewer->has_presented_frame(),d.received_millihz/1000.,d.decoded_millihz/1000.,d.presented_millihz/1000.,d.age_expired,d.sequence_gaps,d.queue_overflows,d.transit_extra_us,viewer->stats().c_str(),client.tcp_diagnostics(peer).c_str());reported=now;}
 }
 std::printf("END declined=%d frames=%u presented=%d pictures=%u last_seq=%llu\n",declined.load(),frames.load(),viewer->has_presented_frame(),presented_pictures,shown_sequence);
 client.cancel_warm(peer,session);client.stop();viewer->request_stop();ShowWindow(window,SW_HIDE);
 auto closing=std::async(std::launch::async,[&]{viewer->close();});while(closing.wait_for(std::chrono::milliseconds(0))!=std::future_status::ready)pump();closing.get();DestroyWindow(window);return shown?0:1; // A picture is connectivity evidence only, never a smoothness pass.
}
