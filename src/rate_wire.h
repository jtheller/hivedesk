#pragma once
#include "hivedesk/types.h"

namespace hive::rate_wire {
// Optional, capability-gated extensions shared by the authenticated network
// channel and local desktop broker. Callers still validate their final length.
template<class W32,class W64>
void write_feedback(const StreamFeedback& f,W32 word,W64 wide) {
    word(f.transport_details?2:1);wide(f.feedback_sequence);wide(f.received_bytes);wide(f.received_frames);
    word(f.received_kbps);word(f.received_millihz);word(f.receive_sample_age_us);
    word(f.service_sample_age_us);word(f.transit_extra_us);
    wide(f.sequence_gaps);wide(f.age_expired);wide(f.queue_overflows);
    if(f.transport_details){wide(f.transport_bytes);wide(f.transport_sample_us);}
}
template<class R32,class R64>
bool read_feedback(StreamFeedback& f,R32 word,R64 wide) {
    const auto version=word();if(version!=1&&version!=2)return false;f.rate_details=true;f.transport_details=version==2;
    f.feedback_sequence=wide();f.received_bytes=wide();f.received_frames=wide();
    f.received_kbps=word();f.received_millihz=word();f.receive_sample_age_us=word();
    f.service_sample_age_us=word();f.transit_extra_us=word();
    f.sequence_gaps=wide();f.age_expired=wide();f.queue_overflows=wide();
    if(f.transport_details){f.transport_bytes=wide();f.transport_sample_us=wide();}return true;
}
template<class W32,class W64>
void write_diagnostics(const SourceDiagnostics& d,W32 word,W64 wide) {
    word(1);word((d.automatic_cbr?1:0)|(d.quality_limited?2:0)|(d.receiver_recover?4:0)|(d.encode_details?8:0));
    word(d.target_bitrate_bps);word(d.actual_bitrate_bps);word(d.average_qp);word(d.rate_reason);
    word(d.receiver_service_us);word(d.receiver_queue_us);
    wide(d.feedback_sequence);wide(d.source_discarded);
    wide(d.sequence_gaps);wide(d.age_expired);wide(d.queue_overflows);
}
template<class R32,class R64>
bool read_diagnostics(SourceDiagnostics& d,R32 word,R64 wide) {
    if(word()!=1)return false;const auto flags=word();if(flags>15)return false;
    d.rate_details=true;d.automatic_cbr=(flags&1)!=0;d.quality_limited=(flags&2)!=0;d.receiver_recover=(flags&4)!=0;d.encode_details=(flags&8)!=0;
    d.target_bitrate_bps=word();d.actual_bitrate_bps=word();d.average_qp=word();d.rate_reason=word();
    d.receiver_service_us=word();d.receiver_queue_us=word();
    d.feedback_sequence=wide();d.source_discarded=wide();
    d.sequence_gaps=wide();d.age_expired=wide();d.queue_overflows=wide();return true;
}
}
