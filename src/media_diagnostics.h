#pragma once
#include "hivedesk/types.h"
#include <windows.h>
#include <functional>
#include <atomic>
#include <mutex>
#include <thread>

namespace hive {
// Only fixed installed probes. No shell, arbitrary arguments, file access or input.
MediaProbeReport run_media_probe(uint32_t mode,const std::string& source,
    const std::function<bool()>& cancelled);
// Also used by the generated process-lifecycle regression test.
MediaProbeReport bounded_probe_process(const std::wstring& executable,const std::wstring& arguments,
    DWORD timeout_ms,const std::function<bool()>& cancelled);
class MediaDiagnostics {
    std::mutex mutex_;
    std::thread worker_;
    std::atomic<bool> stop_{false},busy_{false};
    uint64_t last_start_=0;
public:
    ~MediaDiagnostics(){stop();}
    void stop();
    void request(uint64_t id,uint32_t mode,const std::string& source,
        std::function<bool()> allowed,std::function<void(MediaProbeReport)> reply);
};
}
