#include "task_lane.h"
#include <future>
#include <vector>
#include <cstdio>

int main(){
    hive::TaskLane lane;
    std::promise<void> entered,release;
    auto gate=release.get_future().share();
    std::vector<int> order;
    lane.post([&]{entered.set_value();gate.wait();order.push_back(0);});
    if(entered.get_future().wait_for(std::chrono::seconds(2))!=std::future_status::ready)return 1;
    lane.post([&]{order.push_back(2);});
    lane.post([&]{order.push_back(1);},true);
    lane.post([&]{order.push_back(3);});
    release.set_value();lane.stop();
    if(order!=std::vector<int>{0,1,2,3}){std::puts("FAIL priority selection or retirement drain");return 1;}
    lane.stop();
    std::puts("PASS foreground priority, background FIFO, complete shutdown drain and idempotent join");
}
