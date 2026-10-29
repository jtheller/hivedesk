#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <utility>

namespace hive {
enum class ReturnResult : uint8_t { Leaf, Returned, Busy, Stale, Limit, Timeout };
struct ReturnRequest { uint64_t session=0,id=0;uint8_t hops=16; };
struct ReturnReply { uint64_t session=0,id=0;ReturnResult result=ReturnResult::Stale; };

// UI-thread state machine. A reply may close only the exact view observed when
// this operation began. Never fall back to closing a parent after a timeout.
class NestedReturn {
public:
    struct View {
        std::string peer;uint64_t session=0,generation=0;
        bool operator==(const View&)const=default;
    };
    struct Parent { std::string peer;ReturnRequest request; };
    struct Completion { View view;Parent parent;ReturnResult result;bool close=false; };
    struct Pending { View view;Parent parent;ReturnRequest request;uint64_t deadline=0; };
    bool busy()const{return pending_.has_value();}
    const std::optional<Pending>& pending()const{return pending_;}
    ReturnRequest begin(View view,Parent parent,uint8_t hops,uint64_t now) {
        // Caller rejects busy admission before begin, including cyclic routes.
        ReturnRequest request{view.session,++sequence_,hops};
        pending_=Pending{std::move(view),std::move(parent),request,now+10000};return request;
    }
    std::optional<Completion> reply(const std::string& peer,const ReturnReply& reply,const View& current,bool parent_valid) {
        if(!pending_||peer!=pending_->view.peer||reply.session!=pending_->request.session||reply.id!=pending_->request.id)return {};
        const auto result=current==pending_->view&&parent_valid?reply.result:ReturnResult::Stale;
        return finish(result,result==ReturnResult::Leaf);
    }
    std::optional<Completion> tick(uint64_t now,const View& current,bool parent_valid) {
        if(!pending_)return {};
        if(current!=pending_->view||!parent_valid)return finish(ReturnResult::Stale,false);
        if(now>=pending_->deadline)return finish(ReturnResult::Timeout,false);
        return {};
    }
    std::optional<Completion> cancel(){return pending_?finish(ReturnResult::Stale,false):std::optional<Completion>{};}
private:
    std::optional<Completion> finish(ReturnResult result,bool close) {
        Completion done{pending_->view,pending_->parent,close?ReturnResult::Returned:result,close};pending_.reset();return done;
    }
    uint64_t sequence_=0;
    std::optional<Pending> pending_;
};
}
