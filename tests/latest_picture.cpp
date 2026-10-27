#include "../src/latest_picture.h"
#include <atomic>
#include <cstdio>
#include <future>
#include <stdexcept>
#include <thread>

using namespace std::chrono_literals;
namespace {
void require(bool condition,const char* message){if(!condition)throw std::runtime_error(message);}

void bounded_ownership(){
    hive::LatestPicture pictures;
    const auto first=pictures.reserve(),second=pictures.reserve(),third=pictures.reserve();
    require(first&&second&&third&&*first<3&&*second<3&&*third<3&&
        *first!=*second&&*first!=*third&&*second!=*third,"Three exclusive conversion slots were not available");
    require(!pictures.reserve(),"Picture handoff allocated an unbounded fourth slot");
    pictures.release(*second);
    const auto replacement=pictures.reserve();
    require(replacement&&*replacement==*second,"Cancelled conversion slot was not reusable");
    require(!pictures.publish(3).accepted,"Out-of-range publication accepted");
    const auto published=pictures.publish(*first);
    require(published.accepted&&!published.replaced,"First ready picture incorrectly replaced an image");
    require(!pictures.publish(*first).accepted,"Duplicate publication stole a ready slot");
    const auto visible=pictures.take();
    require(visible&&*visible==*first&&!pictures.take(visible),"Taking without a replacement lost the current picture");
    require(!pictures.reserve(),"Presenter-owned texture became writable without a replacement");
    pictures.release(*visible);pictures.release(*replacement);pictures.release(*third);
}

void blocked_presenter_keeps_only_latest(){
    hive::LatestPicture pictures;std::array<std::atomic<uint64_t>,3> payload{};
    const auto initial=pictures.reserve();require(bool(initial),"Initial picture reservation failed");
    payload[*initial]=1;require(pictures.publish(*initial).accepted,"Initial picture publication failed");
    const auto visible=pictures.take();require(bool(visible),"Initial presenter ownership failed");
    constexpr uint64_t Last=10000;
    auto producer=std::async(std::launch::async,[&]{
        for(uint64_t sequence=2;sequence<=Last;++sequence){
            const auto slot=pictures.reserve();
            if(!slot||*slot==*visible||*slot>=payload.size())return false;
            payload[*slot].store(sequence,std::memory_order_relaxed);
            const auto publication=pictures.publish(*slot);
            if(!publication.accepted||publication.replaced!=(sequence>2))return false;
        }
        return true;
    });
    bool preserved=true;
    while(producer.wait_for(0ms)!=std::future_status::ready){
        preserved=preserved&&payload[*visible].load(std::memory_order_relaxed)==1;
        std::this_thread::yield();
    }
    const bool produced=producer.get();
    require(produced&&preserved&&payload[*visible]==1,"Blocked presenter texture was overwritten or blocked conversion");
    const auto newest=pictures.take(visible);
    require(newest&&payload[*newest]==Last,"Display stall replayed an intermediate picture instead of the latest one");
    require(!pictures.take(newest),"A burst left a FIFO tail after taking its newest image");
    const auto reusable=pictures.reserve();
    require(reusable&&*reusable!=*newest,"Atomic handoff did not release the previous presenter slot");
    pictures.release(*reusable);pictures.release(*newest);
}

void concurrent_atomic_handoff(){
    hive::LatestPicture pictures;std::array<std::atomic<uint64_t>,3> payload{};
    const auto first=pictures.reserve();require(bool(first),"Concurrent initial reservation failed");
    payload[*first]=1;require(pictures.publish(*first).accepted,"Concurrent initial publication failed");
    const auto initial=pictures.take();require(bool(initial),"Concurrent initial take failed");
    constexpr uint64_t Last=50000;
    std::atomic<bool> failed=false,started=false;
    auto consumer=std::async(std::launch::async,[&]{
        auto current=initial;uint64_t seen=1;started=true;
        const auto deadline=std::chrono::steady_clock::now()+5s;
        while(!failed&&seen<Last&&std::chrono::steady_clock::now()<deadline){
            // Keep the current picture while a still-drawing backbuffer waits.
            // It must remain immutable until take atomically exchanges owners.
            if(payload[*current].load(std::memory_order_relaxed)!=seen){failed=true;break;}
            if(const auto next=pictures.take(current)){
                current=next;const auto value=payload[*current].load(std::memory_order_relaxed);
                if(value<=seen){failed=true;break;}
                seen=value;
            }
            std::this_thread::yield();
        }
        const bool ok=!failed&&seen==Last&&payload[*current].load(std::memory_order_relaxed)==Last&&!pictures.take(current);
        pictures.release(*current);return ok;
    });
    while(!started)std::this_thread::yield();
    bool reserved=true;
    for(uint64_t sequence=2;sequence<=Last&&!failed;++sequence){
        const auto slot=pictures.reserve();
        // With one mapper and one presenter an atomic replacement always
        // leaves a writable slot, including during a busy-present retry.
        if(!slot||*slot>=payload.size()){reserved=false;failed=true;break;}
        payload[*slot].store(sequence,std::memory_order_relaxed);
        if(!pictures.publish(*slot).accepted){reserved=false;failed=true;break;}
        if(sequence%17==0)std::this_thread::yield();
    }
    const bool consumed=consumer.get();
    require(reserved&&consumed,"Concurrent latest handoff exposed two presenter owners, changed owned pixels, or replayed stale output");
}

void publication_wakes_waiter(){
    hive::LatestPicture pictures;const auto writing=pictures.reserve();require(bool(writing),"Waiter reservation failed");
    std::promise<void> entered;auto waiting=entered.get_future();bool announced=false;
    auto waiter=std::async(std::launch::async,[&]{return pictures.wait_ready([&]{
        if(!announced){announced=true;entered.set_value();}return false;
    });});
    waiting.wait();
    const bool blocked=waiter.wait_for(10ms)==std::future_status::timeout;
    const bool published=pictures.publish(*writing).accepted;
    const bool woke=waiter.wait_for(1s)==std::future_status::ready;
    if(!woke)pictures.stop();const bool ready=waiter.get();
    require(blocked&&published&&woke&&ready,"Publication failed to wake an empty display waiter");
    const auto selected=pictures.take();require(selected&&*selected==*writing,"Woken waiter lost its sole ready image");
    pictures.release(*selected);
}

void stop_and_cancel(){
    hive::LatestPicture stopped;
    std::promise<void> entered;auto waiting=entered.get_future();bool announced=false;
    auto waiter=std::async(std::launch::async,[&]{return stopped.wait_ready([&]{
        if(!announced){announced=true;entered.set_value();}return false;
    });});
    waiting.wait();stopped.stop();stopped.stop();
    const bool woke=waiter.wait_for(1s)==std::future_status::ready;
    require(woke&&!waiter.get()&&!stopped.reserve()&&!stopped.take(),"Stop did not wake and close the picture mailbox");

    hive::LatestPicture pending;
    const auto old=pending.reserve();require(old&&pending.publish(*old).accepted,"Pending-stop initial picture failed");
    const auto presenting=pending.take();const auto ready=pending.reserve();
    require(presenting&&ready&&pending.publish(*ready).accepted,"Pending-stop ownership setup failed");
    const auto writing=pending.reserve();require(bool(writing),"Pending-stop mapper slot unavailable");
    pending.stop();
    require(!pending.publish(*writing).accepted&&!pending.take(presenting)&&!pending.reserve(),"Stop published or replayed an in-flight/stale image");
    pending.release(*presenting);

    hive::LatestPicture cancelled;std::atomic<bool> cancel=false;
    std::promise<void> cancel_entered;auto cancel_waiting=cancel_entered.get_future();bool cancel_announced=false;
    auto cancellable=std::async(std::launch::async,[&]{return cancelled.wait_ready([&]{
        if(!cancel_announced){cancel_announced=true;cancel_entered.set_value();}return cancel.load();
    });});
    cancel_waiting.wait();cancel=true;
    const bool cancelled_promptly=cancellable.wait_for(1s)==std::future_status::ready;
    if(!cancelled_promptly)cancelled.stop();
    const bool cancelled_result=cancellable.get();
    require(cancelled_promptly&&!cancelled_result,"Cancellation without publication did not end display wait");
}
}

int main(){try{
    bounded_ownership();blocked_presenter_keeps_only_latest();concurrent_atomic_handoff();publication_wakes_waiter();stop_and_cancel();
    std::puts("PASS latest picture: exactly three ownership slots, latest-only burst replacement, immutable presenter texture, atomic concurrent handoff without mapper starvation, publication wakeup, stop and cancellation.");
    return 0;
}catch(const std::exception& error){std::printf("FAIL latest picture: %s\n",error.what());return 1;}}
