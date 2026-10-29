#pragma once
#include "types.h"
#include "video_send_diagnostics.h"
#include "input_diagnostics.h"
#include <filesystem>
#include <memory>
namespace hive {
// Enabled by default, independent of the visual overlay and per-frame trace.
// Local diagnostic counters only. No images, clipboard contents or input events.
class Telemetry {
public:
    enum class Side { View, Host };
    explicit Telemetry(std::filesystem::path directory);
    ~Telemetry();
    void enabled(bool value);
    void select(Side,const std::string& peer,uint64_t session,const std::string& name={});
    void clear(Side);
    void disconnected(const std::string& peer);
    void health(Side,const std::string& peer,const StreamHealth&);
    void source(Side,const std::string& peer,const SourceDiagnostics&);
    void viewer(const std::string& peer,const ViewerDiagnostics&,uint32_t width,uint32_t height);
    void video_send(const std::string& peer,uint64_t session,const VideoSendDiagnostics&);
    void input(const std::string& peer,uint64_t session,const InputDiagnostics&);
    void size(Side,const std::string& peer,uint64_t session,uint32_t width,uint32_t height);
private:
    struct Impl;std::unique_ptr<Impl> impl_;
};
}
