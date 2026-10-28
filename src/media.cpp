#include "hivedesk/media.h"
#include "hivedesk/experience_trace.h"
#include "hivedesk/diagnostic_window.h"
#include "hivedesk/config.h"
#include "hivedesk/adaptive.h"
#include "hivedesk/auto_rate.h"
#include "hivedesk/desktop_session.h"
#include "hivedesk/cursor_composition.h"
#include "media_cuda.h"
#include "frame_timer.h"
#include "source_queue.h"
#include "capture_updates.h"
#include "capture_sequence.h"
#include "viewer_queue.h"
#include "latest_picture.h"
#include <d3d11.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <ffnvcodec/dynlink_cuda.h>
#include <ffnvcodec/dynlink_cuviddec.h>
#include <ffnvcodec/dynlink_nvcuvid.h>
#include <ffnvcodec/nvEncodeAPI.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <future>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <utility>

namespace hive {
namespace {
using Microsoft::WRL::ComPtr;
using Clock = std::chrono::steady_clock;
uint64_t now_us() { return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(Clock::now().time_since_epoch()).count()); }
void hr(HRESULT result, const char* what) {
    if (FAILED(result)) { std::ostringstream message; message << what << " failed: HRESULT 0x" << std::hex << static_cast<unsigned long>(result); throw std::runtime_error(message.str()); }
}
void nv(NVENCSTATUS result, const char* what) {
    if (result != NV_ENC_SUCCESS) throw std::runtime_error(std::string(what) + " failed: NVENC " + std::to_string(result));
}
void cu(CUresult result, const char* what) {
    if (result != CUDA_SUCCESS) throw std::runtime_error(std::string(what) + " failed: CUDA " + std::to_string(result));
}
class Library {
    HMODULE handle_ = nullptr;
public:
    explicit Library(const wchar_t* name) {
        handle_ = LoadLibraryExW(name, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!handle_) throw std::runtime_error("Required NVIDIA driver DLL unavailable; Win32 " + std::to_string(GetLastError()));
    }
    ~Library() { if (handle_) FreeLibrary(handle_); }
    Library(const Library&) = delete;
    template<class F> F* get(const char* name) {
        auto address = GetProcAddress(handle_, name);
        if (!address) throw std::runtime_error(std::string("Missing driver function: ") + name);
        return reinterpret_cast<F*>(address);
    }
};
struct Cuda {
    using SetCurrent = CUresult CUDAAPI(CUcontext);
    Library library{L"nvcuda.dll"};
    tcuInit* init = library.get<tcuInit>("cuInit");
    tcuDeviceGet* get_device = library.get<tcuDeviceGet>("cuDeviceGet");
    tcuCtxCreate_v2* create = library.get<tcuCtxCreate_v2>("cuCtxCreate_v2");
    tcuCtxDestroy_v2* destroy = library.get<tcuCtxDestroy_v2>("cuCtxDestroy_v2");
    SetCurrent* set_current = library.get<SetCurrent>("cuCtxSetCurrent");
    tcuMemAllocPitch_v2* alloc_pitch = library.get<tcuMemAllocPitch_v2>("cuMemAllocPitch_v2");
    tcuMemFree_v2* free_mem = library.get<tcuMemFree_v2>("cuMemFree_v2");
    tcuMemcpy2D_v2* copy2d = library.get<tcuMemcpy2D_v2>("cuMemcpy2D_v2");
    CUcontext context = nullptr;
    explicit Cuda(IDXGIAdapter* adapter = nullptr) {
        cu(init(0), "cuInit"); CUdevice device = 0;
        if (adapter) {
            using D3DDevice = CUresult CUDAAPI(CUdevice*, IDXGIAdapter*);
            cu(library.get<D3DDevice>("cuD3D11GetDevice")(&device, adapter), "Find NVIDIA CUDA device for primary/display adapter");
        } else cu(get_device(&device, 0), "cuDeviceGet");
        cu(create(&context, 0, device), "cuCtxCreate");
    }
    ~Cuda() { if (context) destroy(context); }
    void current() { cu(set_current(context), "cuCtxSetCurrent"); }
};
struct Image {
    uint32_t width = 0, height = 0;
    std::vector<uint8_t> bgra;
};
// Only diagnostic tests call this; the streaming pixel path stays on the GPU.
Image read_texture(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11Texture2D* texture) {
    D3D11_TEXTURE2D_DESC desc{}; texture->GetDesc(&desc);
    desc.BindFlags = 0; desc.MiscFlags = 0; desc.Usage = D3D11_USAGE_STAGING; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging; hr(device->CreateTexture2D(&desc, nullptr, &staging), "Create diagnostic staging texture");
    context->CopyResource(staging.Get(), texture);
    Image result{desc.Width, desc.Height, std::vector<uint8_t>(size_t(desc.Width) * desc.Height * 4)};
    D3D11_MAPPED_SUBRESOURCE mapped{}; hr(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped), "Read diagnostic texture");
    for (uint32_t y = 0; y < desc.Height; ++y)
        std::memcpy(result.bgra.data() + size_t(y) * desc.Width * 4, static_cast<const uint8_t*>(mapped.pData) + size_t(y) * mapped.RowPitch, size_t(desc.Width) * 4);
    context->Unmap(staging.Get(), 0); return result;
}

class Encoder {
public:
    // Four physical IO pairs meet NVIDIA's asynchronous allocation guidance.
    // Only two may be reserved/submitted: storage capacity is not a playback queue.
    static constexpr uint32_t buffer_count = 4, max_in_flight = 2;
    struct Completion {
        EncodedFrame frame;
        uint32_t color_us = 0, nvenc_us = 0, total_us = 0;
        uint32_t average_qp = UINT32_MAX; // HEVC 8-bit QP 0..51; sentinel if unavailable.
        uint64_t submitted_us = 0, completed_us = 0;
    };
private:
    struct Slot {
        CUdeviceptr pixels = 0;
        size_t pitch = 0;
        NV_ENC_REGISTERED_PTR registered = nullptr;
        NV_ENC_INPUT_PTR mapped = nullptr;
        NV_ENC_OUTPUT_PTR output = nullptr;
        HANDLE event = nullptr;
        bool event_registered = false, in_use = false, submitted = false, event_consumed = false, locked = false;
        bool output_ready = false;
        uint64_t sequence = 0, capture_time_us = 0, started_us = 0, converted_us = 0, submitted_us = 0;
        CursorComposition cursor_composition=CursorComposition::Unknown;
    };
    Cuda cuda_;
    std::unique_ptr<HiveGpu, decltype(&hive_gpu_destroy)> gpu_{hive_gpu_create(), hive_gpu_destroy};
    Library library_{L"nvEncodeAPI64.dll"};
    NV_ENCODE_API_FUNCTION_LIST api_{};
    NV_ENC_CONFIG config_{};
    NV_ENC_INITIALIZE_PARAMS initialize_{};
    void* session_ = nullptr;
    Slot slots_[buffer_count]{};
    HANDLE eos_event_ = nullptr;
    bool eos_registered_ = false, initialized_ = false, async_ = false;
    bool automatic_rate_ = false, dynamic_rate_ = false;
    uint32_t nominal_millihz_ = 0;
    mutable std::mutex queue_mutex_;
    std::condition_variable queue_cv_;
    std::deque<uint32_t> pending_;
    uint32_t in_flight_ = 0, next_slot_ = 0;
    bool finishing_ = false, eos_sent_ = false;
    std::exception_ptr failure_;
    uint32_t width_, height_;
    static uint32_t elapsed(uint64_t end, uint64_t begin) {
        return uint32_t(std::min<uint64_t>(10000000, end >= begin ? end - begin : 0));
    }
    static uint32_t single_frame_vbv(uint32_t bitrate_bps, uint32_t millihz) {
        // VBV is in bits. Keep fractional refresh rates (59.94/99.95 Hz)
        // instead of rounding the encoder's nominal cadence down to whole Hz.
        return uint32_t(std::max<uint64_t>(1, (uint64_t(bitrate_bps) * 1000 + millihz - 1) / millihz));
    }
    void register_event(HANDLE& event, bool& registered) {
        event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!event) throw std::runtime_error("Create NVENC completion event failed: Win32 " + std::to_string(GetLastError()));
        NV_ENC_EVENT_PARAMS params{}; params.version = NV_ENC_EVENT_PARAMS_VER; params.completionEvent = event;
        nv(api_.nvEncRegisterAsyncEvent(session_, &params), "NVENC register completion event");
        registered = true;
    }
    void close_event(HANDLE& event, bool& registered) noexcept {
        if (registered) {
            NV_ENC_EVENT_PARAMS params{}; params.version = NV_ENC_EVENT_PARAMS_VER; params.completionEvent = event;
            api_.nvEncUnregisterAsyncEvent(session_, &params); registered = false;
        }
        if (event) { CloseHandle(event); event = nullptr; }
    }
    static void wait_event(HANDLE event) {
        // A device failure must surface as an error, not as a dead worker. There
        // is no NVENC cancellation primitive; shutdown normally drains <=2 jobs.
        const DWORD result = WaitForSingleObject(event, 5000);
        if (result == WAIT_OBJECT_0) return;
        if (result == WAIT_TIMEOUT) throw std::runtime_error("NVENC completion timed out after 5 seconds.");
        throw std::runtime_error("Wait for NVENC completion failed: Win32 " + std::to_string(GetLastError()));
    }
    void cleanup() noexcept {
        if (cuda_.context) cuda_.set_current(cuda_.context);
        if (session_) {
            // The owner has already joined its output thread. Drain orphaned
            // work here too (for example, an exception in a downstream callback).
            try { finish(); } catch (...) {}
            Slot* outstanding[buffer_count]{}; uint32_t count = 0;
            for (auto& slot : slots_) if (slot.submitted) outstanding[count++] = &slot;
            // Slots rotate independently of frame order. Even exceptional
            // teardown must lock outstanding outputs in submission order.
            std::sort(outstanding, outstanding + count, [](const Slot* a, const Slot* b) { return a->sequence < b->sequence; });
            for (uint32_t index = 0; index < count; ++index) {
                auto& slot = *outstanding[index];
                try {
                    if (async_ && !slot.event_consumed) { wait_event(slot.event); slot.event_consumed = true; }
                    if (!slot.locked) {
                        NV_ENC_LOCK_BITSTREAM locked{}; locked.version = NV_ENC_LOCK_BITSTREAM_VER;
                        locked.outputBitstream = slot.output; locked.doNotWait = async_ ? 1 : 0;
                        slot.locked = api_.nvEncLockBitstream(session_, &locked) == NV_ENC_SUCCESS;
                    }
                    if (slot.locked) { api_.nvEncUnlockBitstream(session_, slot.output); slot.locked = false; }
                } catch (...) {}
                slot.submitted = false;
            }
            for (auto& slot : slots_) {
                if (slot.mapped) { api_.nvEncUnmapInputResource(session_, slot.mapped); slot.mapped = nullptr; }
            }
            if (async_ && eos_sent_) { try { wait_event(eos_event_); } catch (...) {} }
            for (auto& slot : slots_) {
                close_event(slot.event, slot.event_registered);
                if (slot.output) { api_.nvEncDestroyBitstreamBuffer(session_, slot.output); slot.output = nullptr; }
                if (slot.registered) { api_.nvEncUnregisterResource(session_, slot.registered); slot.registered = nullptr; }
            }
            close_event(eos_event_, eos_registered_);
            api_.nvEncDestroyEncoder(session_); session_ = nullptr;
        }
        for (auto& slot : slots_) if (slot.pixels) { cuda_.free_mem(slot.pixels); slot.pixels = 0; }
    }
public:
    Encoder(uint32_t width, uint32_t height, const StreamSettings& settings, IDXGIAdapter* adapter = nullptr, bool diagnostic_sync = false) : cuda_(adapter), width_(width), height_(height) {
        try {
            if (!width || !height || width > 8192 || height > 8192 || (width & 1) || (height & 1))
                throw std::runtime_error("HEVC444 requires even screen dimensions up to 8192 in this version.");
            using MaxVersion = NVENCSTATUS NVENCAPI(uint32_t*);
            using Create = NVENCSTATUS NVENCAPI(NV_ENCODE_API_FUNCTION_LIST*);
            uint32_t supported = 0; nv(library_.get<MaxVersion>("NvEncodeAPIGetMaxSupportedVersion")(&supported), "NVENC API version");
            if (supported < ((NVENCAPI_MAJOR_VERSION << 4) | NVENCAPI_MINOR_VERSION)) throw std::runtime_error("NVENC API 11.1 is unavailable; this build targets R500-series or newer NVIDIA drivers.");
            api_.version = NV_ENCODE_API_FUNCTION_LIST_VER;
            nv(library_.get<Create>("NvEncodeAPICreateInstance")(&api_), "NVENC create API");
            NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS open{}; open.version = NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;
            open.device = cuda_.context; open.deviceType = NV_ENC_DEVICE_TYPE_CUDA; open.apiVersion = NVENCAPI_VERSION;
            nv(api_.nvEncOpenEncodeSessionEx(&open, &session_), "NVENC open CUDA session");
            NV_ENC_CAPS_PARAM caps{}; caps.version = NV_ENC_CAPS_PARAM_VER; caps.capsToQuery = NV_ENC_CAPS_SUPPORT_YUV444_ENCODE;
            int support = 0; nv(api_.nvEncGetEncodeCaps(session_, NV_ENC_CODEC_HEVC_GUID, &caps, &support), "NVENC HEVC444 capabilities");
            if (!support) throw std::runtime_error("HEVC 4:4:4 encoding unsupported; 4:2:0 fallback is disabled.");
            caps.capsToQuery = NV_ENC_CAPS_ASYNC_ENCODE_SUPPORT;
            nv(api_.nvEncGetEncodeCaps(session_, NV_ENC_CODEC_HEVC_GUID, &caps, &support), "NVENC asynchronous capabilities");
            async_ = support != 0 && !diagnostic_sync;
            caps.capsToQuery = NV_ENC_CAPS_SUPPORT_DYN_BITRATE_CHANGE;
            nv(api_.nvEncGetEncodeCaps(session_, NV_ENC_CODEC_HEVC_GUID, &caps, &support), "NVENC dynamic bitrate capabilities");
            automatic_rate_ = settings.session && !settings.diagnostic_constqp;
            dynamic_rate_ = automatic_rate_ && support != 0;
            NV_ENC_PRESET_CONFIG preset{}; preset.version = NV_ENC_PRESET_CONFIG_VER; preset.presetCfg.version = NV_ENC_CONFIG_VER;
            nv(api_.nvEncGetEncodePresetConfigEx(session_, NV_ENC_CODEC_HEVC_GUID, NV_ENC_PRESET_P1_GUID,
                 NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY, &preset), "NVENC low-latency preset");
            config_ = preset.presetCfg;
            auto& config = config_;
            config.profileGUID = NV_ENC_HEVC_PROFILE_FREXT_GUID;
            const uint32_t rate = settings.session?std::max(1u,settings.receiver_millihz/1000):(settings.fps ? settings.fps : 120);
            nominal_millihz_ = settings.session ? std::clamp(settings.receiver_millihz, 1000u, 4000000u) : rate * 1000;
            config.gopLength = settings.session?NVENC_INFINITE_GOPLENGTH:rate; config.frameIntervalP = 1;
            config.rcParams.rateControlMode = NV_ENC_PARAMS_RC_CBR;
            if (!settings.session&&(!settings.bitrate_mbps || settings.bitrate_mbps > 4000)) throw std::runtime_error("Bitrate must be 1..4000 Mbps.");
            config.rcParams.averageBitRate = automatic_rate_ ? initial_auto_bitrate(width, height, nominal_millihz_) : settings.bitrate_mbps * 1000000u;
            config.rcParams.maxBitRate = config.rcParams.averageBitRate;
            config.rcParams.vbvBufferSize = single_frame_vbv(config.rcParams.averageBitRate, nominal_millihz_);
            config.rcParams.vbvInitialDelay = config.rcParams.vbvBufferSize;
            config.rcParams.enableLookahead = 0; config.rcParams.enableAQ = 0;
            config.rcParams.enableTemporalAQ = 0; config.rcParams.zeroReorderDelay = 1;
            config.rcParams.lookaheadDepth = 0; config.rcParams.multiPass = NV_ENC_MULTI_PASS_DISABLED;
            config.rcParams.lowDelayKeyFrameScale = 1;
            if (automatic_rate_) {
                // Quality protection is an initial policy, not a claim that a
                // QP ceiling is visually lossless. No filler: an idle desktop
                // need not consume the configured active-picture budget.
                // Retain quality headroom above the QP18 reference without
                // spending spare CBR budget on QP2-4 desktop frames. No filler
                // means easy content may use fewer bits than the target.
                config.rcParams.enableMinQP = 1; config.rcParams.minQP = {AutoRateMinQp, AutoRateMinQp, AutoRateMinQp};
                config.rcParams.enableMaxQP = 1; config.rcParams.maxQP = {AutoRateMaxQp, AutoRateMaxQp, AutoRateMaxQp};
                config.rcParams.enableInitialRCQP = 1; config.rcParams.initialRCQP = {AutoRateTargetQp, AutoRateTargetQp, AutoRateTargetQp};
            } else if(settings.session) {
                // Developer-only reference path for controlled CBR comparisons.
                config.rcParams.rateControlMode=NV_ENC_PARAMS_RC_CONSTQP;
                config.rcParams.constQP={18,18,18};
                config.rcParams.averageBitRate=config.rcParams.maxBitRate=0;
                config.rcParams.vbvBufferSize=config.rcParams.vbvInitialDelay=0;
                config.rcParams.enableMinQP=config.rcParams.enableMaxQP=config.rcParams.enableInitialRCQP=0;
                config.rcParams.multiPass=NV_ENC_MULTI_PASS_DISABLED;
            }
            auto& hevc = config.encodeCodecConfig.hevcConfig;
            hevc.chromaFormatIDC = 3; hevc.repeatSPSPPS = 1; hevc.idrPeriod = config.gopLength;
            hevc.enableFillerDataInsertion = 0;
            hevc.pixelBitDepthMinus8 = 0;
            auto& vui = hevc.hevcVUIParameters;
            vui.videoSignalTypePresentFlag = 1; vui.videoFormat = 5; vui.videoFullRangeFlag = 1;
            vui.colourDescriptionPresentFlag = 1;
            vui.colourPrimaries = 1;
            vui.transferCharacteristics = 1;
            vui.colourMatrix = 1;
            auto& initialize = initialize_; initialize.version = NV_ENC_INITIALIZE_PARAMS_VER;
            initialize.encodeGUID = NV_ENC_CODEC_HEVC_GUID; initialize.presetGUID = NV_ENC_PRESET_P1_GUID;
            initialize.encodeWidth = width; initialize.encodeHeight = height;
            initialize.darWidth = width; initialize.darHeight = height;
            initialize.frameRateNum = nominal_millihz_; initialize.frameRateDen = 1000;
            initialize.enablePTD = 1; initialize.enableEncodeAsync = async_ ? 1 : 0; initialize.encodeConfig = &config;
            initialize.maxEncodeWidth = width; initialize.maxEncodeHeight = height;
            initialize.tuningInfo = NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY;
            nv(api_.nvEncInitializeEncoder(session_, &initialize), "NVENC initialize HEVC444");
            initialized_ = true;
            if (async_) register_event(eos_event_, eos_registered_);
            for (auto& slot : slots_) {
                cu(cuda_.alloc_pitch(&slot.pixels, &slot.pitch, width, size_t(height) * 3, 16), "CUDA YUV444 allocation");
                NV_ENC_REGISTER_RESOURCE resource{}; resource.version = NV_ENC_REGISTER_RESOURCE_VER;
                resource.resourceType = NV_ENC_INPUT_RESOURCE_TYPE_CUDADEVICEPTR;
                resource.width = width; resource.height = height; resource.pitch = static_cast<uint32_t>(slot.pitch);
                resource.resourceToRegister = reinterpret_cast<void*>(slot.pixels); resource.bufferFormat = NV_ENC_BUFFER_FORMAT_YUV444;
                resource.bufferUsage = NV_ENC_INPUT_IMAGE;
                nv(api_.nvEncRegisterResource(session_, &resource), "NVENC register YUV444"); slot.registered = resource.registeredResource;
                NV_ENC_CREATE_BITSTREAM_BUFFER bitstream{}; bitstream.version = NV_ENC_CREATE_BITSTREAM_BUFFER_VER;
                nv(api_.nvEncCreateBitstreamBuffer(session_, &bitstream), "NVENC bitstream buffer"); slot.output = bitstream.bitstreamBuffer;
                if (async_) register_event(slot.event, slot.event_registered);
            }
        } catch (...) { cleanup(); throw; }
    }
    ~Encoder() { cleanup(); }
    Encoder(const Encoder&) = delete;
    Encoder& operator=(const Encoder&) = delete;
    bool ready() const {
        std::lock_guard lock(queue_mutex_);
        if (failure_) std::rethrow_exception(failure_);
        return !finishing_ && in_flight_ < max_in_flight;
    }
    uint32_t in_flight() const { std::lock_guard lock(queue_mutex_); return in_flight_; }
    bool asynchronous() const { return async_; }
    bool automatic_rate() const { return automatic_rate_; }
    bool dynamic_rate() const { return dynamic_rate_; }
    uint32_t bitrate_bps() const { return config_.rcParams.averageBitRate; }
    uint32_t nominal_millihz() const { return nominal_millihz_; }
    // Producer-only, including accessors above. Once idle there can be no
    // consumer NVENC call in progress: receive() releases its slot only after
    // UnlockBitstream/UnmapInputResource. A busy pipeline is retried later,
    // never drained by injecting EOS or rebuilt just to change its budget.
    bool reconfigure(uint32_t bitrate_bps, uint32_t frame_rate_millihz) {
        {
            std::lock_guard lock(queue_mutex_);
            if (failure_) std::rethrow_exception(failure_);
            if (!dynamic_rate_ || finishing_ || in_flight_) return false;
        }
        if (!bitrate_bps || bitrate_bps > 1000000000u || frame_rate_millihz < 1000 || frame_rate_millihz > 4000000)
            throw std::runtime_error("Automatic NVENC rate configuration is out of range.");
        if (bitrate_bps == config_.rcParams.averageBitRate && frame_rate_millihz == nominal_millihz_) return true;
        auto candidate = config_;
        candidate.rcParams.averageBitRate = candidate.rcParams.maxBitRate = bitrate_bps;
        candidate.rcParams.vbvBufferSize = single_frame_vbv(bitrate_bps, frame_rate_millihz);
        candidate.rcParams.vbvInitialDelay = candidate.rcParams.vbvBufferSize;
        NV_ENC_RECONFIGURE_PARAMS change{}; change.version = NV_ENC_RECONFIGURE_PARAMS_VER;
        change.reInitEncodeParams = initialize_;
        change.reInitEncodeParams.encodeConfig = &candidate;
        change.reInitEncodeParams.frameRateNum = frame_rate_millihz;
        // No IDR, state reset, chroma/size/GOP changes, or encoder recreation.
        // Preserve references and the RC history across ordinary adjustments.
        cuda_.current();
        nv(api_.nvEncReconfigureEncoder(session_, &change), "NVENC reconfigure automatic bitrate");
        config_ = candidate; nominal_millihz_ = frame_rate_millihz;
        initialize_.frameRateNum = frame_rate_millihz;
        return true;
    }
    // One producer owns conversion/submission; one consumer owns receive(). The
    // queue lock never spans a CUDA/encoder call or a completion wait.
    bool submit_texture(ID3D11Texture2D* texture, uint64_t sequence, bool force, uint64_t captured_us, bool* completion_queued = nullptr,
        CursorComposition cursor_composition=CursorComposition::Unknown) {
        if (completion_queued) *completion_queued = false;
        const auto index = reserve();
        if (index == buffer_count) return false;
        auto& slot = slots_[index];
        try {
            cuda_.current(); slot.started_us = now_us();
            if (!hive_gpu_encode_texture(gpu_.get(), texture, width_, height_, slot.pixels, slot.pitch))
                throw std::runtime_error(hive_gpu_error(gpu_.get()));
            slot.converted_us = now_us();
            submit_loaded(index, sequence, force, captured_us, completion_queued, cursor_composition);
            return true;
        } catch (...) { abandon(index); throw; }
    }
    bool receive(Completion& completion) {
        uint32_t index = 0;
        {
            std::unique_lock lock(queue_mutex_);
            queue_cv_.wait(lock, [&] {
                return failure_ || (finishing_ && pending_.empty()) ||
                    (!pending_.empty() && slots_[pending_.front()].output_ready);
            });
            if (failure_) std::rethrow_exception(failure_);
            if (pending_.empty()) return false;
            index = pending_.front();
        }
        auto& slot = slots_[index];
        try {
            cuda_.current();
            if (async_) { wait_event(slot.event); slot.event_consumed = true; }
            NV_ENC_LOCK_BITSTREAM locked{}; locked.version = NV_ENC_LOCK_BITSTREAM_VER;
            locked.outputBitstream = slot.output;
            // Async completion is already signaled. Sync fallback blocks only
            // on this consumer, while the producer prepares/submits the next job.
            locked.doNotWait = async_ ? 1 : 0;
            nv(api_.nvEncLockBitstream(session_, &locked), "NVENC read bitstream");
            slot.locked = true;
            try {
                const auto* bytes = static_cast<const uint8_t*>(locked.bitstreamBufferPtr);
                completion.frame.data.assign(bytes, bytes + locked.bitstreamSizeInBytes);
                completion.frame.width = width_; completion.frame.height = height_;
                completion.frame.sequence = slot.sequence; completion.frame.capture_time_us = slot.capture_time_us;
                completion.frame.cursor_composition=slot.cursor_composition;
                completion.frame.keyframe = locked.pictureType == NV_ENC_PIC_TYPE_IDR;
                completion.average_qp = locked.frameAvgQP <= 51 ? locked.frameAvgQP : UINT32_MAX;
            } catch (...) {
                if (api_.nvEncUnlockBitstream(session_, slot.output) == NV_ENC_SUCCESS) { slot.locked = false; slot.submitted = false; }
                throw;
            }
            nv(api_.nvEncUnlockBitstream(session_, slot.output), "NVENC unlock bitstream");
            slot.locked = false; slot.submitted = false;
            nv(api_.nvEncUnmapInputResource(session_, slot.mapped), "NVENC unmap input"); slot.mapped = nullptr;
            completion.completed_us = now_us();
            completion.frame.source_ready_us = completion.completed_us;
            completion.submitted_us = slot.submitted_us;
            completion.color_us = elapsed(slot.converted_us, slot.started_us);
            completion.nvenc_us = elapsed(completion.completed_us, slot.converted_us);
            completion.total_us = elapsed(completion.completed_us, slot.started_us);
            {
                std::lock_guard lock(queue_mutex_);
                pending_.pop_front(); slot.in_use = false; --in_flight_;
            }
            queue_cv_.notify_all();
            return true;
        } catch (...) {
            { std::lock_guard lock(queue_mutex_); failure_ = std::current_exception(); }
            queue_cv_.notify_all(); throw;
        }
    }
    // Producer-only, once no more submit calls can begin. Already submitted
    // reference frames are drained even during cancellation; never freed early.
    void finish() {
        {
            std::lock_guard lock(queue_mutex_);
            if (finishing_) return;
            finishing_ = true;
        }
        queue_cv_.notify_all();
        if (!initialized_) return;
        try {
            cuda_.current();
            NV_ENC_PIC_PARAMS picture{}; picture.version = NV_ENC_PIC_PARAMS_VER;
            picture.encodePicFlags = NV_ENC_PIC_FLAG_EOS; picture.completionEvent = async_ ? eos_event_ : nullptr;
            nv(api_.nvEncEncodePicture(session_, &picture), "NVENC finish encoder"); eos_sent_ = true;
            { std::lock_guard lock(queue_mutex_); for (auto index : pending_) slots_[index].output_ready = true; }
            queue_cv_.notify_all();
        } catch (...) {
            { std::lock_guard lock(queue_mutex_); failure_ = std::current_exception(); }
            queue_cv_.notify_all(); throw;
        }
    }
    EncodedFrame encode(const Image& image, uint64_t sequence, bool force) {
        if (image.width != width_ || image.height != height_ || image.bgra.size() != size_t(width_) * height_ * 4)
            throw std::runtime_error("Encoder image dimensions mismatch.");
        const auto index = reserve();
        if (index == buffer_count) throw std::runtime_error("Encoder diagnostic called with a full or finished pipeline.");
        auto& slot = slots_[index];
        try {
            cuda_.current(); slot.started_us = now_us();
            if (!hive_gpu_encode_host(gpu_.get(), image.bgra.data(), width_, height_, slot.pixels, slot.pitch))
                throw std::runtime_error(hive_gpu_error(gpu_.get()));
            slot.converted_us = now_us(); submit_loaded(index, sequence, force, slot.started_us);
        } catch (...) { abandon(index); throw; }
        Completion completion;
        if (!receive(completion)) throw std::runtime_error("Encoder diagnostic output unexpectedly ended.");
        return std::move(completion.frame);
    }
    EncodedFrame encode_texture(ID3D11Texture2D* texture, uint64_t sequence, bool force,uint32_t* color_us=nullptr,uint32_t* nvenc_us=nullptr) {
        if (!submit_texture(texture, sequence, force, now_us()))
            throw std::runtime_error("Encoder diagnostic called with a full or finished pipeline.");
        Completion completion;
        if (!receive(completion)) throw std::runtime_error("Encoder diagnostic output unexpectedly ended.");
        if (color_us) *color_us = completion.color_us;
        if (nvenc_us) *nvenc_us = completion.nvenc_us;
        return std::move(completion.frame);
    }
private:
    uint32_t reserve() {
        std::lock_guard lock(queue_mutex_);
        if (failure_) std::rethrow_exception(failure_);
        if (finishing_ || in_flight_ >= max_in_flight) return buffer_count;
        for (uint32_t offset = 0; offset < buffer_count; ++offset) {
            const uint32_t index = (next_slot_ + offset) % buffer_count;
            if (!slots_[index].in_use) {
                slots_[index].in_use = true; slots_[index].event_consumed = false;
                slots_[index].output_ready = false;
                ++in_flight_; next_slot_ = (index + 1) % buffer_count;
                return index;
            }
        }
        throw std::runtime_error("Encoder slot accounting failed.");
    }
    void abandon(uint32_t index) noexcept {
        auto& slot = slots_[index];
        // Accepted submissions remain owned by the consumer (or cleanup after
        // an allocation failure while publishing the queue entry).
        if (slot.submitted) return;
        if (slot.mapped) { api_.nvEncUnmapInputResource(session_, slot.mapped); slot.mapped = nullptr; }
        { std::lock_guard lock(queue_mutex_); slot.in_use = false; --in_flight_; }
        queue_cv_.notify_all();
    }
    void submit_loaded(uint32_t index, uint64_t sequence, bool force, uint64_t captured_us, bool* completion_queued = nullptr,
        CursorComposition cursor_composition=CursorComposition::Unknown) {
        auto& slot = slots_[index];
        NV_ENC_MAP_INPUT_RESOURCE mapped{}; mapped.version = NV_ENC_MAP_INPUT_RESOURCE_VER; mapped.registeredResource = slot.registered;
        nv(api_.nvEncMapInputResource(session_, &mapped), "NVENC map input");
        slot.mapped = mapped.mappedResource;
        slot.sequence = sequence; slot.capture_time_us = captured_us;
        slot.cursor_composition=cursor_composition;
        NV_ENC_PIC_PARAMS picture{}; picture.version = NV_ENC_PIC_PARAMS_VER;
        picture.inputBuffer = slot.mapped; picture.bufferFmt = NV_ENC_BUFFER_FORMAT_YUV444;
        picture.inputWidth = width_; picture.inputHeight = height_; picture.inputPitch = static_cast<uint32_t>(slot.pitch);
        picture.outputBitstream = slot.output; picture.completionEvent = async_ ? slot.event : nullptr;
        picture.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;
        picture.inputTimeStamp = sequence; picture.frameIdx = static_cast<uint32_t>(sequence);
        if (force) picture.encodePicFlags = NV_ENC_PIC_FLAG_FORCEIDR | NV_ENC_PIC_FLAG_OUTPUT_SPSPPS;
        slot.submitted_us = now_us();
        const auto status = api_.nvEncEncodePicture(session_, &picture);
        if (status != NV_ENC_SUCCESS && status != NV_ENC_ERR_NEED_MORE_INPUT) nv(status, "NVENC encode HEVC444");
        slot.submitted = true;
        {
            std::lock_guard lock(queue_mutex_); pending_.push_back(index);
            // Even a later error belongs to the drain once publication succeeds.
            if (completion_queued) *completion_queued = true;
            // NEED_MORE_INPUT must not be locked in synchronous mode until a
            // later successful submit or EOS releases the buffered pictures.
            if (status == NV_ENC_SUCCESS) for (auto queued : pending_) slots_[queued].output_ready = true;
        }
        queue_cv_.notify_one();
        if (status == NV_ENC_ERR_NEED_MORE_INPUT)
            throw std::runtime_error("NVENC unexpectedly buffered a frame with B-frames/lookahead disabled; ending the session to preserve the two-frame latency bound.");
    }
};

class Decoder {
public:
    using Output=std::function<void(uint64_t,size_t,uint32_t,uint32_t,HiveGpu*)>;
    struct AsyncOutput {
        Output prepare;
        // Runs after Unmap to publish independent RGB pixels. The display
        // worker owns availability waits outside decode service timing.
        std::function<void(uint32_t,uint32_t)> complete;
        std::function<bool()> admit;
    };
private:
    Cuda cuda_;
    std::unique_ptr<HiveGpu, decltype(&hive_gpu_destroy)> gpu_{hive_gpu_create(), hive_gpu_destroy};
    Library library_{L"nvcuvid.dll"};
    tcuvidCreateVideoParser* create_parser_ = library_.get<tcuvidCreateVideoParser>("cuvidCreateVideoParser");
    tcuvidDestroyVideoParser* destroy_parser_ = library_.get<tcuvidDestroyVideoParser>("cuvidDestroyVideoParser");
    tcuvidParseVideoData* parse_ = library_.get<tcuvidParseVideoData>("cuvidParseVideoData");
    tcuvidGetDecoderCaps* caps_ = library_.get<tcuvidGetDecoderCaps>("cuvidGetDecoderCaps");
    tcuvidCreateDecoder* create_decoder_ = library_.get<tcuvidCreateDecoder>("cuvidCreateDecoder");
    tcuvidDestroyDecoder* destroy_decoder_ = library_.get<tcuvidDestroyDecoder>("cuvidDestroyDecoder");
    tcuvidDecodePicture* decode_ = library_.get<tcuvidDecodePicture>("cuvidDecodePicture");
    tcuvidMapVideoFrame64* map_ = library_.get<tcuvidMapVideoFrame64>("cuvidMapVideoFrame64");
    tcuvidUnmapVideoFrame64* unmap_ = library_.get<tcuvidUnmapVideoFrame64>("cuvidUnmapVideoFrame64");
    CUvideoparser parser_ = nullptr;
    CUvideodecoder decoder_ = nullptr;
    uint32_t width_ = 0, height_ = 0, coded_width_ = 0, coded_height_ = 0, surfaces_ = 0;
    uint32_t map_us_ = 0;
    std::function<void(uint64_t, size_t, uint32_t, uint32_t, HiveGpu*)> output_;
    std::string callback_error_;
    struct Job {CUVIDPARSERDISPINFO picture{};uint32_t width=0,height=0;AsyncOutput output;};
    bool asynchronous_=false;
    std::function<bool()> cancelled_;
    std::mutex jobs_mutex_;
    std::condition_variable jobs_changed_;
    std::deque<Job> jobs_;
    std::vector<int> held_indices_;
    std::thread mapping_worker_;
    bool stopping_=false;
    unsigned outstanding_=0;
    std::exception_ptr failure_;
    // Parser callbacks are synchronous and owned by the one submission thread.
    AsyncOutput* submitting_=nullptr;
    bool submitted_=false;
    CUvideotimestamp submitting_timestamp_=0;
    bool cancelled() const{return cancelled_&&cancelled_();}
    template<class Ready> bool wait_jobs(std::unique_lock<std::mutex>& lock,Ready ready){
        while(!stopping_&&!cancelled()&&!ready())jobs_changed_.wait_for(lock,std::chrono::milliseconds(2));
        if(failure_)std::rethrow_exception(failure_);
        return !stopping_&&!cancelled();
    }
    void release_index(int index){
        {std::lock_guard lock(jobs_mutex_);const auto found=std::find(held_indices_.begin(),held_indices_.end(),index);if(found!=held_indices_.end())held_indices_.erase(found);}
        jobs_changed_.notify_all();
    }
    void map_picture(const CUVIDPARSERDISPINFO& picture,uint32_t width,uint32_t height,const Output& output,uint32_t& map_cost,uint32_t& service_cost){
        CUVIDPROCPARAMS process{};process.progressive_frame=picture.progressive_frame;
        process.top_field_first=picture.top_field_first;process.unpaired_field=1;
        unsigned long long device=0;unsigned int pitch=0;const auto began=now_us();
        cu(map_(decoder_,picture.picture_index,&device,&pitch,&process),"Map decoded YUV444");
        map_cost=uint32_t(std::min<uint64_t>(10000000,now_us()-began));
        try{if(output)output(device,pitch,width,height,gpu_.get());}
        catch(...){unmap_(decoder_,device);throw;}
        cu(unmap_(decoder_,device),"Unmap decoded YUV444");
        service_cost=uint32_t(std::min<uint64_t>(1000000,now_us()-began));
    }
    void mapping_loop() noexcept{
        SetThreadDescription(GetCurrentThread(),L"HiveDesk NVDEC mapping / color");
        try{
            cuda_.current();
            for(;;){
                Job job;
                {
                    std::unique_lock lock(jobs_mutex_);
                    if(!wait_jobs(lock,[&]{return !jobs_.empty();}))break;
                    job=std::move(jobs_.front());jobs_.pop_front();
                }
                uint32_t map_cost=0,service_cost=0;
                map_picture(job.picture,job.width,job.height,job.output.prepare,map_cost,service_cost);
                // Reuse of this decode index is safe after Unmap, even if the
                // prepared RGB texture is still waiting for a display slot.
                release_index(job.picture.picture_index);
                if(job.output.complete)job.output.complete(map_cost,service_cost);
                {std::lock_guard lock(jobs_mutex_);--outstanding_;}
                jobs_changed_.notify_all();
            }
        }catch(...){std::lock_guard lock(jobs_mutex_);failure_=std::current_exception();stopping_=true;}
        {
            std::lock_guard lock(jobs_mutex_);stopping_=true;
            jobs_.clear();held_indices_.clear();outstanding_=0;
        }
        jobs_changed_.notify_all();
        cuda_.set_current(nullptr);
    }
    template<class F> int guarded(F&& callback) noexcept {
        try { return callback(); } catch (const std::exception& error) { callback_error_ = error.what(); return 0; }
        catch (...) { callback_error_ = "Unknown NVDEC callback error"; return 0; }
    }
    static int CUDAAPI sequence(void* self, CUVIDEOFORMAT* format) {
        auto& decoder = *static_cast<Decoder*>(self);
        return decoder.guarded([&] { return decoder.sequence(*format); });
    }
    int sequence(const CUVIDEOFORMAT& format) {
        if (format.codec != cudaVideoCodec_HEVC || format.chroma_format != cudaVideoChromaFormat_444 ||
            format.bit_depth_luma_minus8 != 0 || format.bit_depth_chroma_minus8 != 0)
            throw std::runtime_error("Stream is not HEVC 8-bit 4:4:4; refusing format fallback.");
        if (!format.video_signal_description.video_full_range_flag || format.video_signal_description.matrix_coefficients != 1)
            throw std::runtime_error("Stream color metadata is not full-range BT.709.");
        const uint32_t width = static_cast<uint32_t>(format.display_area.right - format.display_area.left);
        const uint32_t height = static_cast<uint32_t>(format.display_area.bottom - format.display_area.top);
        if (!width || !height || width > 8192 || height > 8192 || (width & 1) || (height & 1))
            throw std::runtime_error("Invalid decoded picture dimensions.");
        if (decoder_ && width_ == width && height_ == height && coded_width_ == format.coded_width && coded_height_ == format.coded_height)
            return static_cast<int>(surfaces_);
        if(asynchronous_){
            std::unique_lock lock(jobs_mutex_);
            // One reservation belongs to the packet invoking this callback.
            // Finish older map/render jobs before replacing their decoder.
            if(!wait_jobs(lock,[&]{return outstanding_==1;}))throw std::runtime_error("NVDEC sequence change cancelled");
        }
        if (decoder_) { cu(destroy_decoder_(decoder_), "Destroy previous NVDEC decoder"); decoder_ = nullptr; }
        width_ = width; height_ = height; coded_width_ = format.coded_width; coded_height_ = format.coded_height;
        surfaces_ = std::max(2u, static_cast<unsigned int>(format.min_num_decode_surfaces));
        if(asynchronous_)surfaces_+=2; // Held outputs must not consume the parser's minimum DPB.
        CUVIDDECODECREATEINFO create{};
        create.CodecType = cudaVideoCodec_HEVC; create.ChromaFormat = cudaVideoChromaFormat_444;
        create.OutputFormat = cudaVideoSurfaceFormat_YUV444; create.bitDepthMinus8 = 0;
        create.ulWidth = format.coded_width; create.ulHeight = format.coded_height;
        create.ulTargetWidth = width; create.ulTargetHeight = height;
        create.display_area.left = static_cast<short>(format.display_area.left);
        create.display_area.top = static_cast<short>(format.display_area.top);
        create.display_area.right = static_cast<short>(format.display_area.right);
        create.display_area.bottom = static_cast<short>(format.display_area.bottom);
        create.ulNumDecodeSurfaces = surfaces_; create.ulNumOutputSurfaces = 2;
        create.DeinterlaceMode = cudaVideoDeinterlaceMode_Weave; create.ulCreationFlags = cudaVideoCreate_PreferCUVID;
        cu(create_decoder_(&decoder_, &create), "Create HEVC444 hardware decoder");
        return static_cast<int>(surfaces_);
    }
    static int CUDAAPI decode(void* self, CUVIDPICPARAMS* picture) {
        auto& decoder = *static_cast<Decoder*>(self);
        return decoder.guarded([&] {
            if(decoder.asynchronous_){
                std::unique_lock lock(decoder.jobs_mutex_);
                if(!decoder.wait_jobs(lock,[&]{return std::find(decoder.held_indices_.begin(),decoder.held_indices_.end(),picture->CurrPicIdx)==decoder.held_indices_.end();}))
                    throw std::runtime_error("NVDEC picture reuse cancelled");
            }
            cu(decoder.decode_(decoder.decoder_, picture), "NVDEC decode picture"); return 1;
        });
    }
    static int CUDAAPI display(void* self, CUVIDPARSERDISPINFO* picture) {
        auto& decoder = *static_cast<Decoder*>(self);
        return decoder.guarded([&] {
            if (!picture) return 1;
            if(decoder.asynchronous_){
                if(!decoder.submitting_||decoder.submitted_||picture->timestamp!=decoder.submitting_timestamp_)
                    throw std::runtime_error("Unexpected reordered NVDEC output in low-latency stream");
                {
                    std::lock_guard lock(decoder.jobs_mutex_);
                    if(decoder.stopping_)throw std::runtime_error("NVDEC output cancelled");
                    decoder.held_indices_.push_back(picture->picture_index);
                    decoder.jobs_.push_back(Job{*picture,decoder.width_,decoder.height_,std::move(*decoder.submitting_)});
                    decoder.submitted_=true;
                }
                decoder.jobs_changed_.notify_all();return 1;
            }
            uint32_t service_cost=0;
            decoder.map_picture(*picture,decoder.width_,decoder.height_,decoder.output_,decoder.map_us_,service_cost);
            return 1;
        });
    }
public:
    explicit Decoder(Output output, IDXGIAdapter* adapter = nullptr,bool asynchronous=false,std::function<bool()> cancelled={})
        : cuda_(adapter), output_(std::move(output)),asynchronous_(asynchronous),cancelled_(std::move(cancelled)) {
        CUVIDDECODECAPS caps{}; caps.eCodecType = cudaVideoCodec_HEVC; caps.eChromaFormat = cudaVideoChromaFormat_444;
        cu(caps_(&caps), "NVDEC HEVC444 capabilities");
        if (!caps.bIsSupported || !(caps.nOutputFormatMask & (1u << cudaVideoSurfaceFormat_YUV444)))
            throw std::runtime_error("HEVC444 hardware decoding unsupported; software/420 fallback is disabled.");
        CUVIDPARSERPARAMS parser{}; parser.CodecType = cudaVideoCodec_HEVC;
        parser.ulMaxNumDecodeSurfaces = 1; parser.ulMaxDisplayDelay = 0; parser.ulClockRate = 1000000;
        parser.pUserData = this; parser.pfnSequenceCallback = &Decoder::sequence;
        parser.pfnDecodePicture = &Decoder::decode; parser.pfnDisplayPicture = &Decoder::display;
        cu(create_parser_(&parser_, &parser), "Create HEVC parser");
        try{if(asynchronous_)mapping_worker_=std::thread([this]{mapping_loop();});}
        catch(...){destroy_parser_(parser_);parser_=nullptr;throw;}
    }
    ~Decoder() {
        stop_async();
        cuda_.set_current(cuda_.context);
        if (parser_) destroy_parser_(parser_);
        if (decoder_) destroy_decoder_(decoder_);
    }
    uint32_t map_us() const{return map_us_;}
    void request_stop(){
        {std::lock_guard lock(jobs_mutex_);stopping_=true;}
        jobs_changed_.notify_all();
    }
    void stop_async(){request_stop();if(mapping_worker_.joinable())mapping_worker_.join();}
    bool drain_async(){std::unique_lock lock(jobs_mutex_);return wait_jobs(lock,[&]{return outstanding_==0;});}
    unsigned outstanding(){std::lock_guard lock(jobs_mutex_);return outstanding_;}
    void check_async(){std::lock_guard lock(jobs_mutex_);if(failure_)std::rethrow_exception(failure_);}
    bool feed_async(const EncodedFrame& frame,AsyncOutput output){
        if(!asynchronous_)throw std::runtime_error("Asynchronous feed requires an asynchronous decoder");
        {
            std::unique_lock lock(jobs_mutex_);
            if(!wait_jobs(lock,[&]{return outstanding_<2;}))return false;
            ++outstanding_;
        }
        submitting_=&output;submitted_=false;submitting_timestamp_=static_cast<CUvideotimestamp>(frame.sequence);
        try{
            // Capacity waiting is not permission to decode an expired compressed
            // reference. Recheck the caller's strict queue/epoch admission now.
            if(output.admit&&!output.admit()){
                submitting_=nullptr;
                {std::lock_guard lock(jobs_mutex_);if(outstanding_)--outstanding_;}
                jobs_changed_.notify_all();return true;
            }
            feed(frame);
            if(!submitted_)throw std::runtime_error("Missing immediate NVDEC display callback");
            submitting_=nullptr;return true;
        }catch(...){
            submitting_=nullptr;
            {std::lock_guard lock(jobs_mutex_);if(!submitted_&&outstanding_)--outstanding_;}
            jobs_changed_.notify_all();throw;
        }
    }
    void feed(const EncodedFrame& frame) {
        cuda_.current(); callback_error_.clear();map_us_=0;
        if (frame.data.empty() || frame.data.size() > 64 * 1024 * 1024) throw std::runtime_error("Invalid encoded frame size.");
        CUVIDSOURCEDATAPACKET packet{}; packet.payload = frame.data.data(); packet.payload_size = static_cast<tcu_ulong>(frame.data.size());
        packet.flags = CUVID_PKT_ENDOFPICTURE | CUVID_PKT_TIMESTAMP; packet.timestamp = static_cast<CUvideotimestamp>(frame.sequence);
        auto result = parse_(parser_, &packet);
        if (!callback_error_.empty()) throw std::runtime_error(callback_error_);
        cu(result, "Parse HEVC444 frame");
    }
};

class Capture {
    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<IDXGIOutputDuplication> duplication_;
    ComPtr<ID3D11Texture2D> capture_;
    ComPtr<IDXGIAdapter1> adapter_;
    DXGI_OUTPUT_DESC description_{};
    bool acquired_=false;
    CaptureUpdateFilter updates_;
    CaptureCursorComposition cursor_;
public:
    uint32_t width = 0, height = 0;
    Capture() {
        ComPtr<IDXGIFactory1> factory; hr(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "Create DXGI factory");
        const HMONITOR primary = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
        ComPtr<IDXGIOutput1> output;
        for (UINT a = 0; !output; ++a) {
            ComPtr<IDXGIAdapter1> adapter;
            if (factory->EnumAdapters1(a, &adapter) == DXGI_ERROR_NOT_FOUND) break;
            for (UINT o = 0; ; ++o) {
                ComPtr<IDXGIOutput> candidate;
                if (adapter->EnumOutputs(o, &candidate) == DXGI_ERROR_NOT_FOUND) break;
                DXGI_OUTPUT_DESC desc{}; hr(candidate->GetDesc(&desc), "Get output description");
                if (desc.AttachedToDesktop && desc.Monitor == primary) {
                    hr(candidate.As(&output), "Query primary output"); adapter_ = adapter; description_ = desc; break;
                }
            }
        }
        if (!output) throw std::runtime_error("Primary monitor is not available for capture.");
        ComPtr<IDXGIOutput6> modern_output;
        if (SUCCEEDED(output.As(&modern_output))) {
            DXGI_OUTPUT_DESC1 color{}; hr(modern_output->GetDesc1(&color), "Get primary color space");
            if (color.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 ||
                color.ColorSpace == DXGI_COLOR_SPACE_RGB_STUDIO_G2084_NONE_P2020)
                throw std::runtime_error("HDR primary display is unsupported in this SDR-only version; select an SDR desktop.");
        }
        if (description_.Rotation != DXGI_MODE_ROTATION_IDENTITY && description_.Rotation != DXGI_MODE_ROTATION_UNSPECIFIED)
            throw std::runtime_error("Rotated primary monitors are not supported in this first version.");
        hr(D3D11CreateDevice(adapter_.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
             nullptr, 0, D3D11_SDK_VERSION, &device_, nullptr, &context_), "Create primary capture device");
        hr(output->DuplicateOutput(device_.Get(), &duplication_), "Desktop Duplication");
        width = static_cast<uint32_t>(description_.DesktopCoordinates.right - description_.DesktopCoordinates.left);
        height = static_cast<uint32_t>(description_.DesktopCoordinates.bottom - description_.DesktopCoordinates.top);
        D3D11_TEXTURE2D_DESC texture{}; texture.Width = width; texture.Height = height;
        texture.MipLevels = 1; texture.ArraySize = 1; texture.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        texture.SampleDesc.Count = 1; texture.Usage = D3D11_USAGE_DEFAULT; texture.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        hr(device_->CreateTexture2D(&texture, nullptr, &capture_), "Create capture GPU texture");
    }
    ~Capture(){if(acquired_)duplication_->ReleaseFrame();}
    IDXGIAdapter* adapter() const { return adapter_.Get(); }
    RECT bounds() const { return description_.DesktopCoordinates; }
    ID3D11Texture2D* texture() const { return capture_.Get(); }
    CursorComposition cursor_composition() const {return cursor_.picture();}
    Image readback_for_test() { return read_texture(device_.Get(), context_.Get(), capture_.Get()); }
    bool next() {
        // Retain ownership through encode/pacing, releasing immediately before
        // acquisition so DXGI need not copy intermediate updates we cannot use.
        if(acquired_){acquired_=false;hr(duplication_->ReleaseFrame(),"Release desktop frame");}
        DXGI_OUTDUPL_FRAME_INFO info{}; ComPtr<IDXGIResource> resource;
        const auto result = duplication_->AcquireNextFrame(16, &info, &resource);
        if (result == DXGI_ERROR_WAIT_TIMEOUT) return false;
        hr(result, "Acquire desktop frame");
        acquired_=true;
        // Pointer-only movement needs no video work, but switching between a
        // separate pointer and a cursor embedded in the desktop needs a new
        // image. Bind ownership to the copied image, including recovery IDRs.
        cursor_.observe(info.LastMouseUpdateTime.QuadPart,info.PointerPosition.Visible!=FALSE);
        if(!updates_.needs_copy(info.LastPresentTime.QuadPart)&&!cursor_.needs_copy())return false;
        try {
            ComPtr<ID3D11Texture2D> texture; hr(resource.As(&texture), "Get desktop texture");
            context_->CopyResource(capture_.Get(), texture.Get()); context_->Flush();
            updates_.copied();cursor_.copied();
        } catch (...) { duplication_->ReleaseFrame();acquired_=false;throw; }
        return true;
    }
};

class Renderer {
public:
    struct Picture {
        ComPtr<ID3D11Texture2D> texture;
        ComPtr<ID3D11ShaderResourceView> resource;
        uint32_t width=0,height=0,convert_us=0;
        CursorPresentation cursor;
    };
private:
    HWND window_;
    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<IDXGISwapChain> swap_;
    HANDLE display_ready_=nullptr;
    bool display_credit_=false; // Presenter thread; consumed only by a successful Present.
    bool paced_present_=false;
    ComPtr<ID3D11RenderTargetView> target_;
    Picture synchronous_picture_;
    ComPtr<ID3D11VertexShader> vertex_;
    ComPtr<ID3D11PixelShader> pixel_;
    ComPtr<ID3D11SamplerState> sampler_;
    ComPtr<IDXGIAdapter> adapter_;
    uint32_t window_width_ = 0, window_height_ = 0;
    bool pending_present_ = false;
    uint32_t convert_us_=0,draw_us_=0;
    std::function<HRESULT()> present_for_test_;
    CursorPresentation pending_cursor_;
    std::function<void(const CursorPresentation&)> cursor_presented_;
    void target() {
        ComPtr<ID3D11Texture2D> backbuffer; hr(swap_->GetBuffer(0, IID_PPV_ARGS(&backbuffer)), "Get swapchain backbuffer");
        hr(device_->CreateRenderTargetView(backbuffer.Get(), nullptr, &target_), "Create render target");
    }
public:
    explicit Renderer(HWND window,bool paced=false) : window_(window),paced_present_(paced) {
        if (!IsWindow(window)) throw std::runtime_error("Invalid viewer window.");
        // Match capture's primary-output ownership. A default D3D adapter can
        // differ on hybrid/multiple-GPU machines; NVDEC follows this device.
        ComPtr<IDXGIFactory1> adapters;hr(CreateDXGIFactory1(IID_PPV_ARGS(&adapters)),"Create viewer adapter factory");
        const HMONITOR primary=MonitorFromPoint(POINT{0,0},MONITOR_DEFAULTTOPRIMARY);
        for(UINT a=0;!adapter_;++a){
            ComPtr<IDXGIAdapter1> candidate;
            if(adapters->EnumAdapters1(a,&candidate)==DXGI_ERROR_NOT_FOUND)break;
            for(UINT o=0;;++o){
                ComPtr<IDXGIOutput> output;
                if(candidate->EnumOutputs(o,&output)==DXGI_ERROR_NOT_FOUND)break;
                DXGI_OUTPUT_DESC description{};hr(output->GetDesc(&description),"Get viewer primary output");
                if(description.AttachedToDesktop&&description.Monitor==primary){adapter_=candidate;break;}
            }
        }
        if(!adapter_)throw std::runtime_error("Viewer primary monitor adapter is unavailable");
        DXGI_SWAP_CHAIN_DESC swap{}; swap.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        swap.SampleDesc.Count = 1; swap.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        swap.BufferCount = 2; swap.OutputWindow = window; swap.Windowed = TRUE;
        swap.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        if(paced_present_)swap.Flags=DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
        hr(D3D11CreateDeviceAndSwapChain(adapter_.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
             nullptr, 0, D3D11_SDK_VERSION, &swap, &swap_, &device_, nullptr, &context_), "Create viewer D3D11 device");
        DXGI_SWAP_CHAIN_DESC actual{}; hr(swap_->GetDesc(&actual), "Get swapchain description");
        window_width_ = actual.BufferDesc.Width; window_height_ = actual.BufferDesc.Height; target();
        constexpr char shader[] =
            "struct V{float4 p:SV_POSITION;float2 uv:TEXCOORD0;};"
            "V vs(uint i:SV_VertexID){V o;float2 uv=float2((i<<1)&2,i&2);o.uv=uv;o.p=float4(uv.x*2-1,1-uv.y*2,0,1);return o;}"
            "Texture2D tex:register(t0);SamplerState samp:register(s0);"
            "float4 ps(V i):SV_TARGET{return float4(tex.Sample(samp,i.uv).rgb,1);}";
        ComPtr<ID3DBlob> vs, ps, errors;
        hr(D3DCompile(shader, sizeof(shader)-1, nullptr, nullptr, nullptr, "vs", "vs_5_0", 0, 0, &vs, &errors), "Compile viewer vertex shader");
        hr(D3DCompile(shader, sizeof(shader)-1, nullptr, nullptr, nullptr, "ps", "ps_5_0", 0, 0, &ps, &errors), "Compile viewer pixel shader");
        hr(device_->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &vertex_), "Create vertex shader");
        hr(device_->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &pixel_), "Create pixel shader");
        D3D11_SAMPLER_DESC sample{}; sample.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sample.AddressU = sample.AddressV = sample.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sample.MaxLOD = D3D11_FLOAT32_MAX;
        hr(device_->CreateSamplerState(&sample, &sampler_), "Create viewer sampler");
        ComPtr<IDXGIDevice> dxgi_device; hr(device_.As(&dxgi_device), "Get viewer DXGI device");
        ComPtr<IDXGIDevice1> latency_device; hr(device_.As(&latency_device), "Get low-latency DXGI device");
        hr(latency_device->SetMaximumFrameLatency(1), "Limit present queue to one frame");
        ComPtr<IDXGIFactory> factory; hr(adapter_->GetParent(IID_PPV_ARGS(&factory)), "Get viewer factory");
        hr(factory->MakeWindowAssociation(window, DXGI_MWA_NO_ALT_ENTER), "Disable DXGI Alt-Enter");
        if(paced_present_){
            ComPtr<IDXGISwapChain2> chain;hr(swap_.As(&chain),"Get waitable viewer swapchain");
            hr(chain->SetMaximumFrameLatency(1),"Limit waitable present queue to one frame");
            display_ready_=chain->GetFrameLatencyWaitableObject();
            if(!display_ready_)throw std::runtime_error("Viewer frame-latency waitable object unavailable");
        }
    }
    ~Renderer(){if(display_ready_)CloseHandle(display_ready_);}
    IDXGIAdapter* adapter() const { return adapter_.Get(); }
    uint32_t convert_us() const{return convert_us_;}
    uint32_t draw_us() const{return draw_us_;}
    std::array<uint64_t,6> display_observation() const {
        DXGI_FRAME_STATISTICS value{};const auto result=swap_->GetFrameStatistics(&value);
        UINT submitted=0;swap_->GetLastPresentCount(&submitted);
        return {uint32_t(result),value.PresentCount,value.PresentRefreshCount,value.SyncRefreshCount,uint64_t(value.SyncQPCTime.QuadPart),submitted};
    }
    bool present_pending() const {return pending_present_;}
    bool cancel_pending() {return std::exchange(pending_present_,false);}
    void set_present_for_test(std::function<HRESULT()> callback){present_for_test_=std::move(callback);}
    void set_cursor_presented(std::function<void(const CursorPresentation&)> callback){cursor_presented_=std::move(callback);}
    bool retry_present(const std::function<bool()>& eligible={},uint32_t* present_cost=nullptr) {
        if(present_cost)*present_cost=0;
        if(!pending_present_)return false;
        if(eligible&&!eligible()){pending_present_=false;return false;}
        const auto began=now_us();
        const auto result=present_for_test_?present_for_test_():swap_->Present(paced_present_?1:0,DXGI_PRESENT_DO_NOT_WAIT);
        if(present_cost)*present_cost=uint32_t(std::min<uint64_t>(10000000,now_us()-began));
        // Keep only the latest prepared backbuffer. No NVDEC surface remains
        // mapped and a retry never repeats conversion or queues another frame.
        pending_present_=result==DXGI_ERROR_WAS_STILL_DRAWING||result==DXGI_STATUS_OCCLUDED;
        if(!pending_present_)hr(result,"Present decoded desktop");
        if(result==S_OK)display_credit_=false;
        if(result==S_OK&&pending_cursor_.valid&&cursor_presented_)cursor_presented_(pending_cursor_);
        return result==S_OK;
    }
    bool present_when_ready(const std::function<bool()>& eligible,uint32_t* present_cost=nullptr){
        if(present_cost)*present_cost=0;
        while(pending_present_){
            if(!wait_available([&]{return eligible&&!eligible();})){pending_present_=false;return false;}
            uint32_t cost=0;const bool presented=retry_present(eligible,&cost);
            if(present_cost)*present_cost=cost;
            if(presented||!pending_present_)return presented;
            Sleep(1);
        }
        return false;
    }
    bool wait_available(const std::function<bool()>& cancelled){
        while(!cancelled()){
            // The latency wait consumes an admission signal, not a
            // periodic vsync tick. Dropping a stale/invalidated ready picture
            // or retrying a busy Present must retain the acquired admission.
            // Otherwise no Present replenishes it and a live decoder can wait
            // forever with zero displayed frames after warm activation.
            if(display_credit_||!display_ready_||!IsWindowVisible(window_))return true;
            const auto wait=WaitForSingleObject(display_ready_,2);
            if(wait==WAIT_OBJECT_0){display_credit_=true;return true;}
            if(wait!=WAIT_TIMEOUT)throw std::runtime_error("Wait for viewer display availability");
        }
        return false;
    }
    ComPtr<ID3D11Texture2D> upload_for_test(const Image& image) {
        D3D11_TEXTURE2D_DESC desc{}; desc.Width = image.width; desc.Height = image.height;
        desc.MipLevels = 1; desc.ArraySize = 1; desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1; desc.Usage = D3D11_USAGE_DEFAULT; desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA data{}; data.pSysMem = image.bgra.data(); data.SysMemPitch = image.width * 4;
        ComPtr<ID3D11Texture2D> texture; hr(device_->CreateTexture2D(&desc, &data, &texture), "Upload benchmark texture"); return texture;
    }
    void convert(Picture& picture,uint64_t yuv,size_t pitch,uint32_t width,uint32_t height,HiveGpu* gpu){
        if(picture.width!=width||picture.height!=height){
            D3D11_TEXTURE2D_DESC texture{};texture.Width=width;texture.Height=height;
            texture.MipLevels=1;texture.ArraySize=1;texture.Format=DXGI_FORMAT_B8G8R8A8_UNORM;
            texture.SampleDesc.Count=1;texture.Usage=D3D11_USAGE_DEFAULT;texture.BindFlags=D3D11_BIND_SHADER_RESOURCE;
            Picture replacement;replacement.width=width;replacement.height=height;
            hr(device_->CreateTexture2D(&texture,nullptr,&replacement.texture),"Create viewer picture texture");
            hr(device_->CreateShaderResourceView(replacement.texture.Get(),nullptr,&replacement.resource),"Create viewer picture SRV");
            picture=std::move(replacement);
        }
        const auto begin=now_us();
        if(!hive_gpu_decode_texture(gpu,yuv,pitch,width,height,picture.texture.Get()))throw std::runtime_error(hive_gpu_error(gpu));
        picture.convert_us=uint32_t(std::min<uint64_t>(10000000,now_us()-begin));
    }
    void draw_prepared(const Picture& picture,Image* proof=nullptr,uint32_t* render_cost=nullptr,uint32_t* present_cost=nullptr,bool* presented=nullptr,
        std::function<bool()> eligible={},bool defer_present=false){
        const auto render_begin=now_us();pending_present_=false;convert_us_=picture.convert_us;draw_us_=0;
        if(presented)*presented=false;if(render_cost)*render_cost=0;if(present_cost)*present_cost=0;
        RECT client{};if(!GetClientRect(window_,&client))return;
        const auto width=static_cast<uint32_t>(client.right),height=static_cast<uint32_t>(client.bottom);
        if(!width||!height)return;
        if(width!=window_width_||height!=window_height_){
            context_->OMSetRenderTargets(0,nullptr,nullptr);target_.Reset();
            hr(swap_->ResizeBuffers(0,width,height,DXGI_FORMAT_UNKNOWN,paced_present_?DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT:0),"Resize viewer swapchain");
            window_width_=width;window_height_=height;target();
        }
        const float black[]={0,0,0,1};context_->ClearRenderTargetView(target_.Get(),black);
        auto* target=target_.Get();context_->OMSetRenderTargets(1,&target,nullptr);
        const double scale=std::min(double(width)/picture.width,double(height)/picture.height);
        D3D11_VIEWPORT viewport{};viewport.Width=static_cast<float>(picture.width*scale);viewport.Height=static_cast<float>(picture.height*scale);
        viewport.TopLeftX=(width-viewport.Width)*.5f;viewport.TopLeftY=(height-viewport.Height)*.5f;viewport.MaxDepth=1;
        context_->RSSetViewports(1,&viewport);context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context_->VSSetShader(vertex_.Get(),nullptr,0);context_->PSSetShader(pixel_.Get(),nullptr,0);
        auto* resource=picture.resource.Get();auto* sampler=sampler_.Get();
        context_->PSSetShaderResources(0,1,&resource);context_->PSSetSamplers(0,1,&sampler);context_->Draw(3,0);
        // Mapper only reuses a released slot. Unbind after its final draw so
        // future CUDA maps cannot race an unintended later graphics reference.
        ID3D11ShaderResourceView* empty=nullptr;context_->PSSetShaderResources(0,1,&empty);
        if(proof){ComPtr<ID3D11Texture2D> backbuffer;hr(swap_->GetBuffer(0,IID_PPV_ARGS(&backbuffer)),"Get diagnostic backbuffer");*proof=read_texture(device_.Get(),context_.Get(),backbuffer.Get());}
        draw_us_=uint32_t(std::min<uint64_t>(10000000,now_us()-render_begin));
        if(render_cost)*render_cost=uint32_t(std::min<uint64_t>(10000000,uint64_t(draw_us_)+convert_us_));
        pending_cursor_=picture.cursor;pending_present_=true;
        if(defer_present)return;
        const bool result=retry_present(eligible,present_cost);if(presented)*presented=result;
    }
    void draw(uint64_t yuv,size_t pitch,uint32_t image_width,uint32_t image_height,HiveGpu* gpu,Image* proof=nullptr,
        uint32_t* render_cost=nullptr,uint32_t* present_cost=nullptr,bool* presented=nullptr,std::function<bool()> eligible={},bool defer_present=false,
        CursorPresentation cursor={}){
        convert(synchronous_picture_,yuv,pitch,image_width,image_height,gpu);
        synchronous_picture_.cursor=cursor;
        draw_prepared(synchronous_picture_,proof,render_cost,present_cost,presented,std::move(eligible),defer_present);
    }

};
} // namespace

#include "media_host.inl"

struct MediaViewer::Impl {
    mutable std::mutex mutex;
    std::condition_variable changed;
    ViewerQueue frames;
    bool stopping = true, displayed = false, present_enabled = true;
    std::atomic<bool> display_cadence=false;
    CursorPresentation cursor;
    std::thread worker;
    std::string status = "Closed";
    StreamFeedback feedback{};uint64_t last_arrival=0;
    ViewerServiceWindow service_samples;
    ViewerPresentWindow present_samples;
    ViewerDiagnostics diagnostics{};
    uint64_t sample_since=0,received_count=0,decoded_count=0,presented_count=0,received_bytes=0;
    uint64_t receive_sample_at=0;
    DiagnosticWindow<4> live_window;std::array<uint64_t,4> live_totals{};
    void sync_queue() {
        feedback.recover=frames.recovering();diagnostics.discarded=frames.discarded();
        if(stopping)return;
        // A genuine sustained encoder/transport latency change cannot be
        // distinguished from old TCP backlog by unsynchronized clocks alone.
        // Preserve the old baseline, then use the existing stream/session
        // restart barrier instead of declaring a late IDR fresh by guesswork.
        if(frames.recovery_expired(now_us())){
            status="Media error: Freshness recovery requires a new video session";
            stopping=true;changed.notify_all();return;
        }
        if(feedback.recover)status="Waiting for fresh IDR after stale video";
    }
    void sample(uint64_t now){
        if(!sample_since){sample_since=now;return;}const auto elapsed=now-sample_since;if(elapsed<1000000)return;
        diagnostics.received_millihz=uint32_t(received_count*1000000000ull/elapsed);diagnostics.decoded_millihz=uint32_t(decoded_count*1000000000ull/elapsed);diagnostics.presented_millihz=uint32_t(presented_count*1000000000ull/elapsed);
        diagnostics.receive_kbps=uint32_t(std::min<uint64_t>(UINT32_MAX,received_bytes*8000ull/elapsed));
        receive_sample_at=now;
        received_count=decoded_count=presented_count=received_bytes=0;sample_since=now;
    }
};
MediaViewer::MediaViewer() : impl_(std::make_unique<Impl>()) {}
bool media_prewarm_capacity() {
    ComPtr<IDXGIFactory1> factory;if(FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))return false;
    const auto primary=MonitorFromPoint(POINT{0,0},MONITOR_DEFAULTTOPRIMARY);
    for(UINT i=0;;++i){
        ComPtr<IDXGIAdapter1> adapter;if(FAILED(factory->EnumAdapters1(i,&adapter)))break;
        for(UINT j=0;;++j){
            ComPtr<IDXGIOutput> output;if(FAILED(adapter->EnumOutputs(j,&output)))break;
            DXGI_OUTPUT_DESC desc{};if(FAILED(output->GetDesc(&desc))||desc.Monitor!=primary)continue;
            ComPtr<IDXGIAdapter3> budget_adapter;if(FAILED(adapter.As(&budget_adapter)))return false;
            DXGI_QUERY_VIDEO_MEMORY_INFO memory{};
            if(FAILED(budget_adapter->QueryVideoMemoryInfo(0,DXGI_MEMORY_SEGMENT_GROUP_LOCAL,&memory)))return false;
            // Leave room for the foreground stream and the user's applications.
            // WDDM's current budget includes pressure from other processes.
            const auto reserve=std::max<uint64_t>(256ull<<20,memory.Budget/8);
            return memory.Budget>memory.CurrentUsage&&memory.Budget-memory.CurrentUsage>reserve;
        }
    }
    return false;
}
MediaViewer::~MediaViewer() { close(); }
bool MediaViewer::open(HWND window, std::string& error,std::function<void(const CursorPresentation&)> presented,bool warm) {
    close();
    // DXGI creation may synchronously send window messages. Create it on the
    // caller/UI thread, then hand the device/context to the render worker.
    std::unique_ptr<Renderer> renderer;
    try { renderer = std::make_unique<Renderer>(window,true); }
    catch (const std::exception& problem) { error = problem.what(); return false; }
    renderer->set_cursor_presented([this,presented=std::move(presented)](const CursorPresentation& cursor){
        bool changed=false;
        {
            std::lock_guard guard(impl_->mutex);if(impl_->stopping)return;
            const auto& previous=impl_->cursor;
            changed=!previous.valid||previous.session!=cursor.session||previous.composition!=cursor.composition;
            impl_->cursor=cursor;
        }
        // Cursor motion keeps its independent low-latency lane. Only ownership
        // transitions need UI work; retain the last presented state for startup.
        if(changed&&presented)presented(cursor);
    });
    { std::lock_guard guard(impl_->mutex); impl_->stopping = false; impl_->present_enabled=!warm; impl_->frames.reset(); impl_->displayed = false; impl_->status = "Opening HEVC444 viewer";
        impl_->cursor={};
        impl_->live_window={};impl_->live_totals={};impl_->live_window.sample(now_us(),{});
        impl_->feedback={};impl_->last_arrival=0;impl_->service_samples.clear();impl_->present_samples.clear();impl_->diagnostics={};impl_->sample_since=now_us();impl_->receive_sample_at=0;impl_->received_count=impl_->decoded_count=impl_->presented_count=impl_->received_bytes=0;}
    std::promise<std::string> initialized; auto ready = initialized.get_future();
    impl_->worker = std::thread([this, renderer = std::move(renderer), initialized = std::move(initialized)]() mutable {
        SetThreadDescription(GetCurrentThread(),L"HiveDesk viewer decode / lifetime");
        bool announced=false;
        try{
            std::atomic<bool> pipeline_stop=false;
            LatestPicture pictures;
            struct ReadyPicture {Renderer::Picture pixels;std::shared_ptr<ViewerQueue::Pending> pending;uint64_t prepared_at=0;};
            std::array<ReadyPicture,3> ready_pictures;
            const auto cancelled=[this,&pipeline_stop]{if(pipeline_stop)return true;std::lock_guard guard(impl_->mutex);return impl_->stopping;};
            const auto eligible=[this,&pipeline_stop](const std::shared_ptr<ViewerQueue::Pending>& pending){
                if(pipeline_stop)return false;
                std::lock_guard guard(impl_->mutex);
                const bool fresh=!impl_->stopping&&impl_->frames.output_fresh(*pending,now_us());
                impl_->sync_queue();return fresh&&!impl_->stopping;
            };
            Decoder decoder({},renderer->adapter(),true,cancelled);
            std::thread presenter;
            struct Workers {
                std::atomic<bool>& stop;LatestPicture& pictures;Decoder& decoder;std::thread& presenter;
                std::mutex& state_mutex;std::condition_variable& changed;
                ~Workers(){
                    {std::lock_guard lock(state_mutex);stop=true;}changed.notify_all();
                    pictures.stop();decoder.request_stop();if(presenter.joinable())presenter.join();decoder.stop_async();
                }
            } workers{pipeline_stop,pictures,decoder,presenter,impl_->mutex,impl_->changed};
            presenter=std::thread([&]{
                SetThreadDescription(GetCurrentThread(),L"HiveDesk viewer presentation");
                std::optional<size_t> current;bool current_presented=false;
                try{
                    {
                        std::unique_lock guard(impl_->mutex);
                        impl_->changed.wait(guard,[&]{return pipeline_stop||impl_->stopping||impl_->present_enabled;});
                        if(pipeline_stop||impl_->stopping)return;
                    }
                    FrameTimer present_retry;
                    while(!cancelled()){
                        if(current&&current_presented&&!impl_->display_cadence){renderer->cancel_pending();pictures.release(*current);current.reset();current_presented=false;}
                        if(!current&&!pictures.wait_ready(cancelled))break;
                        // Wait BEFORE acquiring the latest image and before
                        // touching the swapchain backbuffer. Mapping/NVDEC keep
                        // progressing and replace the single ready image here.
                        if(!renderer->wait_available(cancelled))break;
                        const auto latest=pictures.take(current);
                        if(latest){
                            if(current&&!current_presented){std::lock_guard guard(impl_->mutex);++impl_->diagnostics.skipped;}
                            current=latest;current_presented=false;renderer->cancel_pending();
                        }
                        if(!current)continue;
                        auto& picture=ready_pictures[*current];
                        const auto can_present=[&]{return current_presented||eligible(picture.pending);};
                        if(!can_present()){
                            renderer->cancel_pending();pictures.release(*current);current.reset();
                            std::lock_guard guard(impl_->mutex);++impl_->diagnostics.skipped;continue;
                        }
                        uint32_t render_us=0,present_us=0;bool presented=false;
                        if(latest||(current_presented&&!renderer->present_pending()))renderer->draw_prepared(picture.pixels,nullptr,&render_us,&present_us,&presented,can_present);
                        else presented=renderer->retry_present(can_present,&present_us);
                        if(presented&&!current_presented&&experience_enabled()){
                            const auto d=renderer->display_observation();
                            experience_trace(ExperienceEvent::Present,picture.pending->frame.session,picture.pending->frame.sequence,
                                {picture.pending->arrival,picture.prepared_at,render_us,present_us,d[0],d[1],d[2],d[3],d[4],d[5]});
                        }
                        {
                            std::lock_guard guard(impl_->mutex);
                            if(!impl_->stopping){
                                if(latest){impl_->diagnostics.render_us=render_us;impl_->diagnostics.draw_us=renderer->draw_us();}
                                impl_->diagnostics.present_us=present_us;
                                if(presented){if(current_presented)++impl_->diagnostics.repeated_presentations;else{++impl_->presented_count;++impl_->live_totals[2];impl_->present_samples.presented(now_us());}}
                                else if(!renderer->present_pending())++impl_->diagnostics.skipped;
                            }
                        }
                        if(presented&&impl_->display_cadence)current_presented=true;
                        else if(!renderer->present_pending()){pictures.release(*current);current.reset();current_presented=false;}
                        else present_retry.wait_until(Clock::now()+std::chrono::milliseconds(1)); // High-resolution retry, never timer-tick-sized Sleep jitter.
                    }
                }catch(const std::exception& problem){
                    std::lock_guard guard(impl_->mutex);
                    if(!impl_->stopping){impl_->status=std::string("Media error: ")+problem.what();impl_->stopping=true;}
                    impl_->changed.notify_all();
                }
                if(current)pictures.release(*current);
            });
            initialized.set_value({});announced=true;
            while(!cancelled()){
                decoder.check_async();
                std::optional<ViewerQueue::Pending> pending;
                {
                    std::unique_lock guard(impl_->mutex);
                    if(!impl_->stopping&&impl_->frames.empty())impl_->changed.wait_for(guard,std::chrono::milliseconds(2));
                    impl_->sync_queue();if(impl_->stopping)break;
                    pending=impl_->frames.pop(now_us());impl_->sync_queue();
                }
                if(!pending)continue;
                auto context=std::make_shared<ViewerQueue::Pending>(std::move(*pending));
                auto slot=std::make_shared<std::optional<size_t>>();
                Decoder::AsyncOutput output;
                output.admit=[this,context]{
                    std::lock_guard guard(impl_->mutex);
                    const bool fresh=!impl_->stopping&&impl_->frames.admit(*context,now_us());
                    impl_->sync_queue();return fresh&&!impl_->stopping;
                };
                output.prepare=[&,context,slot](uint64_t yuv,size_t pitch,uint32_t width,uint32_t height,HiveGpu* gpu){
                    if(!eligible(context)){std::lock_guard guard(impl_->mutex);++impl_->diagnostics.skipped;return;}
                    const auto reserved=pictures.reserve();
                    if(!reserved){if(cancelled())return;throw std::runtime_error("Viewer picture ownership exhausted");}
                    try{
                        auto& picture=ready_pictures[*reserved];picture.pending=context;
                        // Offscreen conversion only: the mapping worker never
                        // touches the D3D immediate context or waits for vsync.
                        renderer->convert(picture.pixels,yuv,pitch,width,height,gpu);
                        picture.pixels.cursor={context->frame.session,context->frame.sequence,context->frame.cursor_composition,true};
                        *slot=reserved;
                    }catch(...){pictures.release(*reserved);throw;}
                };
                output.complete=[&,context,slot](uint32_t map_us,uint32_t service_us){
                    if(!*slot){
                        std::lock_guard guard(impl_->mutex);
                        if(!impl_->stopping){
                            ++impl_->decoded_count;++impl_->live_totals[1];
                            impl_->diagnostics.decode_us=map_us;impl_->diagnostics.nvdec_map_us=map_us;
                            if(!context->frame.keyframe)impl_->service_samples.add(now_us(),service_us);
                        }
                        return;
                    }
                    const auto index=**slot;const auto& image=ready_pictures[index].pixels;
                    bool prepared=false;
                    {
                        std::lock_guard guard(impl_->mutex);
                        if(!impl_->stopping&&!pipeline_stop){
                            ++impl_->decoded_count;++impl_->live_totals[1];
                            impl_->diagnostics.decode_us=map_us;impl_->diagnostics.nvdec_map_us=map_us;
                            impl_->diagnostics.convert_us=image.convert_us;
                            if(!context->frame.keyframe)impl_->service_samples.add(now_us(),service_us);
                            prepared=impl_->frames.mark_output_ready(*context,now_us());impl_->sync_queue();
                            if(prepared&&!impl_->stopping){
                                impl_->status=std::to_string(image.width)+'x'+std::to_string(image.height)+" HEVC 4:4:4 | NVDEC async | latest ready + display pacing";
                                // Reveal the hidden viewer on prepared pixels;
                                // actual presentation is counted only at S_OK.
                                impl_->displayed=true;
                            }else prepared=false;
                        }
                    }
                    if(!prepared){pictures.release(index);return;}
                    ready_pictures[index].prepared_at=now_us();
                    experience_trace(ExperienceEvent::Prepared,context->frame.session,context->frame.sequence,
                        {context->arrival,ready_pictures[index].prepared_at,map_us,service_us,image.convert_us});
                    const auto published=pictures.publish(index);
                    if(published.replaced){std::lock_guard guard(impl_->mutex);++impl_->diagnostics.skipped;}
                };
                if(!decoder.feed_async(context->frame,std::move(output)))break;
            }
            decoder.check_async();
        }catch(const std::exception& problem){
            if(!announced)initialized.set_value(problem.what());
            std::lock_guard guard(impl_->mutex);
            if(!impl_->stopping){impl_->status=std::string("Media error: ")+problem.what();impl_->stopping=true;}
            impl_->changed.notify_all();
        }
    });
    error = ready.get(); if (!error.empty()) { close(); return false; } return true;
}
void MediaViewer::submit(EncodedFrame frame) {
    experience_trace(ExperienceEvent::Arrival,frame.session,frame.sequence,{frame.capture_time_us,frame.source_ready_us,frame.width,frame.height});
    std::lock_guard guard(impl_->mutex);
    if (impl_->stopping) return;
    if(!impl_->frames.accepts_session(frame.session))return;
    const auto now=now_us();impl_->feedback.session=frame.session;impl_->feedback.capture_time_us=frame.capture_time_us;impl_->last_arrival=now;
    impl_->diagnostics.session=frame.session;++impl_->received_count;++impl_->live_totals[0];impl_->live_totals[3]+=frame.data.size();impl_->received_bytes+=frame.data.size();
    ++impl_->feedback.received_frames;impl_->feedback.received_bytes+=frame.data.size();
    const bool queued=impl_->frames.push(std::move(frame),now);impl_->sync_queue();
    if(queued||impl_->frames.recovery_pending())impl_->changed.notify_one();
}
void MediaViewer::request_stop() {
    { std::lock_guard guard(impl_->mutex); impl_->stopping = true; impl_->frames.reset();impl_->cursor={}; }
    impl_->changed.notify_all();
}
void MediaViewer::close() {
    request_stop();if (impl_->worker.joinable()) impl_->worker.join();
}
void MediaViewer::set_display_cadence(bool enabled){impl_->display_cadence=enabled;}
void MediaViewer::activate(){
    {std::lock_guard guard(impl_->mutex);impl_->present_enabled=true;}
    impl_->changed.notify_all();
}
std::string MediaViewer::stats() const { std::lock_guard guard(impl_->mutex); return impl_->status; }
bool MediaViewer::has_frame() const { std::lock_guard guard(impl_->mutex); return impl_->displayed && !impl_->stopping; }
bool MediaViewer::has_presented_frame() const { std::lock_guard guard(impl_->mutex); return impl_->live_totals[2]!=0 && !impl_->stopping; }
CursorPresentation MediaViewer::cursor_presentation() const {std::lock_guard guard(impl_->mutex);return impl_->cursor;}
StreamFeedback MediaViewer::feedback() const {
    std::lock_guard guard(impl_->mutex);if(impl_->stopping)return {};
    impl_->sync_queue();if(impl_->stopping)return {};
    ++impl_->feedback.feedback_sequence;auto report=impl_->feedback;const auto now=now_us();
    impl_->sample(now);
    if(impl_->last_arrival)report.arrival_age_us=uint32_t(std::min<uint64_t>(UINT32_MAX,now-impl_->last_arrival));
    report.queue_us=uint32_t(std::min<uint64_t>(UINT32_MAX,impl_->frames.feedback_queue_age(now)));
    // Mapping latency is not pipelined capacity. Actual compressed backlog drives throttling.
    report.service_us=0;report.service_sample_age_us=impl_->service_samples.sample_age(now);
    report.rate_details=true;report.received_kbps=impl_->diagnostics.receive_kbps;report.received_millihz=impl_->diagnostics.received_millihz;
    report.receive_sample_age_us=impl_->receive_sample_at?uint32_t(std::min<uint64_t>(UINT32_MAX,now-impl_->receive_sample_at)):UINT32_MAX;
    report.transit_extra_us=impl_->frames.transit_extra_us();
    report.sequence_gaps=impl_->frames.sequence_gaps();report.age_expired=impl_->frames.age_expired();report.queue_overflows=impl_->frames.queue_overflows();
    return report;
}
ViewerDiagnostics MediaViewer::diagnostics() const {
    std::lock_guard guard(impl_->mutex);if(impl_->stopping)return {};
    auto result=impl_->diagnostics;const auto now=now_us();
    const auto rates=impl_->live_window.sample(now,impl_->live_totals);
    result.received_millihz=rates.rate(0,1000000000,4000000);result.decoded_millihz=rates.rate(1,1000000000,4000000);
    result.presented_millihz=rates.rate(2,1000000000,4000000);result.receive_kbps=rates.rate(3,8000);
    result.service_p90_us=impl_->service_samples.p90(now);result.service_sample_age_us=impl_->service_samples.sample_age(now);
    result.queue_us=uint32_t(std::min<uint64_t>(UINT32_MAX,impl_->frames.queue_age(now)));result.queue_depth=uint32_t(impl_->frames.size());result.recovering=impl_->frames.recovering();
    result.present_gap_p95_us=impl_->present_samples.p95(now);result.present_gap_max_us=impl_->present_samples.maximum(now);
    result.receive_sample_age_us=impl_->last_arrival?uint32_t(std::min<uint64_t>(UINT32_MAX,now-impl_->last_arrival)):UINT32_MAX;
    result.transit_extra_us=impl_->frames.transit_extra_us();result.sequence_gaps=impl_->frames.sequence_gaps();
    result.age_expired=impl_->frames.age_expired();result.queue_overflows=impl_->frames.queue_overflows();return result;
}

// Generated pixels only: integration checks can exercise the actual asynchronous
// viewer without capturing the user's desktop or depending on a remote computer.
EncodedFrame media_generated_test_frame() {
    constexpr uint32_t width=640,height=360;
    Image source{width,height,std::vector<uint8_t>(size_t(width)*height*4)};
    for(uint32_t y=0;y<height;++y)for(uint32_t x=0;x<width;++x){auto* p=source.bgra.data()+(size_t(y)*width+x)*4;p[0]=uint8_t(x*255/width);p[1]=uint8_t(y*255/height);p[2]=80;p[3]=255;}
    Encoder encoder(width,height,StreamSettings{60,40});return encoder.encode(source,0,true);
}
static bool codec_self_test(std::string& report,const StreamSettings& settings) {
    try {
        constexpr uint32_t width = 256, height = 192;
        Image source{width, height, std::vector<uint8_t>(size_t(width) * height * 4)};
        const uint8_t colors[][3] = {{255,0,0},{0,0,255},{0,255,0},{255,255,255},{0,0,0},{0,255,255},{255,0,255},{255,255,0}};
        for (uint32_t y = 0; y < height; ++y) for (uint32_t x = 0; x < width; ++x) {
            const auto& color = colors[y < height / 2 ? (x % 2) : (x / 32)];
            auto* pixel = source.bgra.data() + (size_t(y) * width + x) * 4;
            pixel[0] = color[0]; pixel[1] = color[1]; pixel[2] = color[2]; pixel[3] = 255;
        }
        std::vector<Image> reconstructed;
        Encoder encoder(width, height, settings);
        Decoder decoder([&](uint64_t yuv, size_t pitch, uint32_t decoded_width, uint32_t decoded_height, HiveGpu* gpu) {
            Image image{decoded_width, decoded_height, std::vector<uint8_t>(size_t(decoded_width) * decoded_height * 4)};
            if (!hive_gpu_decode_host(gpu, yuv, pitch, decoded_width, decoded_height, image.bgra.data())) throw std::runtime_error(hive_gpu_error(gpu));
            reconstructed.push_back(std::move(image));
        });
        size_t bytes = 0; bool keyframe = false;
        const auto started = Clock::now();
        for (uint64_t sequence = 0; sequence < 4; ++sequence) {
            auto frame = encoder.encode(source, sequence, sequence == 0);
            bytes += frame.data.size(); keyframe = keyframe || frame.keyframe;
            decoder.feed(frame);
        }
        if (reconstructed.size() != 4 || !keyframe) throw std::runtime_error("Hardware roundtrip did not produce all four frames and an IDR.");
        double absolute_error = 0; int maximum = 0;
        for (const auto& image : reconstructed) {
            if (image.width != width || image.height != height) throw std::runtime_error("Decoded dimensions mismatch.");
            for (size_t i = 0; i < size_t(width) * height; ++i) for (size_t c = 0; c < 3; ++c) {
                const int delta = std::abs(int(image.bgra[i * 4 + c]) - int(source.bgra[i * 4 + c]));
                absolute_error += delta; maximum = std::max(maximum, delta);
            }
        }
        const double mean = absolute_error / (reconstructed.size() * size_t(width) * height * 3);
        if (mean > 8.0 || maximum > 70) throw std::runtime_error("Reconstructed color check failed: MAE " + std::to_string(mean) + ", max " + std::to_string(maximum));
        std::ostringstream text;
        text << "PASS: " << (settings.session?(settings.diagnostic_constqp?"diagnostic CQP18; ":"automatic CBR, QP ceiling22; "):"legacy CBR; ") << "actual NVENC HEVC 8-bit 4:4:4 -> NVDEC, 4/4 frames, " << width << 'x' << height
             << ", full-range BT.709 confirmed from parsed stream, one-pixel alternating chroma plus color bars, "
             << bytes << " encoded bytes, RGB MAE=" << std::fixed << std::setprecision(3) << mean << ", max=" << maximum
             << ", elapsed=" << std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started).count()
             << "ms. CUDA GPU color conversions; test images uploaded/read back for verification. This is not a two-PC latency or capture/display test.";
        report = text.str(); return true;
    } catch (const std::exception& error) { report = std::string("FAIL: ") + error.what(); return false; }
}
bool media_self_test(std::string& report){return codec_self_test(report,{60,100});}
bool media_auto_self_test(std::string& report){
    std::string cbr,baseline;
    if(!codec_self_test(cbr,{0,0,59940,1})){report=cbr;return false;}
    if(!codec_self_test(baseline,{0,0,59940,1,true})){report=baseline;return false;}
    report=cbr+"\n"+baseline;return true;
}
bool media_auto_quality_self_test(std::string& report) {
    struct TestWindow { HWND value=nullptr; ~TestWindow(){if(value)DestroyWindow(value);} } window;
    struct DibPage {
        HDC dc=nullptr; HBITMAP bitmap=nullptr; HFONT font=nullptr;
        HGDIOBJ previous_bitmap=nullptr,previous_font=nullptr; void* pixels=nullptr;
        ~DibPage(){
            if(previous_font)SelectObject(dc,previous_font);
            if(previous_bitmap)SelectObject(dc,previous_bitmap);
            if(font)DeleteObject(font);if(bitmap)DeleteObject(bitmap);if(dc)DeleteDC(dc);
        }
    } page;
    std::string phase="generated quality fixture";
    try {
        constexpr uint32_t width=1280,height=720,page_height=1080,frames=60,text_width=960;
        constexpr wchar_t class_name[]=L"HiveDeskAutoQualityTest";
        WNDCLASSW cls{};cls.lpfnWndProc=DefWindowProcW;cls.hInstance=GetModuleHandleW(nullptr);cls.lpszClassName=class_name;
        if(!RegisterClassW(&cls)&&GetLastError()!=ERROR_CLASS_ALREADY_EXISTS)throw std::runtime_error("Cannot register hidden quality test window.");
        window.value=CreateWindowExW(0,class_name,L"HiveDesk hidden quality test",WS_POPUP,-32000,-32000,width,height,nullptr,nullptr,cls.hInstance,nullptr);
        if(!window.value)throw std::runtime_error("Cannot create hidden quality test window.");
        Renderer renderer(window.value);
        page.dc=CreateCompatibleDC(nullptr);
        if(!page.dc)throw std::runtime_error("Cannot create offline text context.");
        BITMAPINFO info{};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);info.bmiHeader.biWidth=width;
        info.bmiHeader.biHeight=-int(page_height);info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;info.bmiHeader.biCompression=BI_RGB;
        page.bitmap=CreateDIBSection(page.dc,&info,DIB_RGB_COLORS,&page.pixels,nullptr,0);
        if(!page.bitmap||!page.pixels)throw std::runtime_error("Cannot allocate generated text page.");
        page.previous_bitmap=SelectObject(page.dc,page.bitmap);
        page.font=CreateFontW(-14,0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,ANTIALIASED_QUALITY,FIXED_PITCH,L"Consolas");
        if(!page.font)throw std::runtime_error("Cannot create generated small-text font.");
        page.previous_font=SelectObject(page.dc,page.font);
        auto* page_pixels=static_cast<uint8_t*>(page.pixels);
        for(uint32_t y=0;y<page_height;++y)for(uint32_t x=0;x<width;++x){
            auto* p=page_pixels+(size_t(y)*width+x)*4;
            if(x<text_width){p[0]=p[1]=p[2]=uint8_t(y/20%2?235:249);}
            else {
                // A permanent one-pixel chroma region catches accidental 4:2:0
                // or color-matrix regressions independently of the text region.
                const bool stripe=x<text_width+64 ? (x&1)!=0 : ((x/5+y/5)&1)!=0;
                p[0]=stripe?220:35;p[1]=uint8_t(40+(y/32%4)*40);p[2]=stripe?35:220;
            }
            p[3]=255;
        }
        SetBkMode(page.dc,TRANSPARENT);
        for(uint32_t row=0;row<page_height/20;++row){
            const COLORREF colors[]{RGB(15,15,15),RGB(180,35,40),RGB(20,65,180),RGB(20,110,55)};
            SetTextColor(page.dc,colors[row%4]);
            std::wostringstream line;
            line<<L"HiveDesk row "<<std::setw(3)<<std::setfill(L'0')<<row
                <<L" | HEVC 4:4:4 | 059.94 FPS | AaBb 0123456789 /\\ [] {} <> == + - _ : ; . ,";
            const auto text=line.str();
            if(!TextOutW(page.dc,8,int(row*20+2),text.c_str(),int(text.size())))throw std::runtime_error("Cannot draw generated text.");
        }
        GdiFlush();
        Image source{width,height,std::vector<uint8_t>(size_t(width)*height*4)};
        auto texture=renderer.upload_for_test(source);
        ComPtr<ID3D11Device> device;texture->GetDevice(&device);
        ComPtr<ID3D11DeviceContext> context;device->GetImmediateContext(&context);
        const auto generate=[&](uint32_t frame){
            for(uint32_t y=0;y<height;++y){
                const uint32_t sy=(y+frame*3)%page_height;
                std::memcpy(source.bgra.data()+size_t(y)*width*4,page_pixels+size_t(sy)*width*4,size_t(width)*4);
            }
            // Detailed moving content changes motion and texture every frame;
            // both modes regenerate precisely the same input pixels.
            const uint32_t left=40+(frame*13)%(text_width-220),top=64+(frame*7)%(height-220);
            for(uint32_t y=0;y<128;++y)for(uint32_t x=0;x<160;++x){
                auto* p=source.bgra.data()+(size_t(top+y)*width+left+x)*4;
                const bool checker=((x/3+y/3)&1)!=0;
                p[0]=checker?230:40;p[1]=uint8_t((x*3+y*2)%200+25);p[2]=checker?45:210;p[3]=255;
            }
        };
        struct Metrics {
            uint64_t bytes=0,qp_sum=0,encode_us=0,pixels=0;
            uint64_t absolute[3]{},squared[3]{},text_squared=0,chroma_squared=0;
            uint32_t maximum[3]{},max_qp=0,decoded=0;
            double seconds=0;
            static double psnr(uint64_t error,uint64_t samples){return error?10.0*std::log10(65025.0*double(samples)/double(error)):99.0;}
            double all_psnr()const{return psnr(squared[0]+squared[1]+squared[2],pixels*3);}
            double text_psnr()const{return psnr(text_squared,uint64_t(frames)*text_width*height*3);}
            double chroma_psnr()const{return psnr(chroma_squared,uint64_t(frames)*(width-text_width)*height*3);}
        };
        const auto exercise=[&](bool constqp){
            phase=constqp?"generated CQP18 baseline":"generated automatic CBR";
            Metrics metrics;
            Encoder encoder(width,height,StreamSettings{0,0,60000,1,constqp},renderer.adapter());
            Decoder decoder([&](uint64_t yuv,size_t pitch,uint32_t w,uint32_t h,HiveGpu* gpu){
                if(w!=width||h!=height)throw std::runtime_error("Generated quality decoder changed dimensions.");
                Image proof{w,h,std::vector<uint8_t>(size_t(w)*h*4)};
                if(!hive_gpu_decode_host(gpu,yuv,pitch,w,h,proof.bgra.data()))throw std::runtime_error(hive_gpu_error(gpu));
                for(uint32_t y=0;y<height;++y)for(uint32_t x=0;x<width;++x){
                    const size_t index=(size_t(y)*width+x)*4;
                    for(uint32_t c=0;c<3;++c){
                        const auto error=uint32_t(std::abs(int(proof.bgra[index+c])-int(source.bgra[index+c])));
                        metrics.absolute[c]+=error;metrics.squared[c]+=uint64_t(error)*error;
                        metrics.maximum[c]=std::max(metrics.maximum[c],error);
                        (x<text_width?metrics.text_squared:metrics.chroma_squared)+=uint64_t(error)*error;
                    }
                }
                metrics.pixels+=uint64_t(width)*height;++metrics.decoded;
            },renderer.adapter());
            const auto began=Clock::now();
            for(uint32_t sequence=0;sequence<frames;++sequence){
                if(Clock::now()-began>std::chrono::seconds(12))throw std::runtime_error("Generated quality comparison exceeded its per-mode time budget.");
                generate(sequence);
                context->UpdateSubresource(texture.Get(),0,nullptr,source.bgra.data(),width*4,0);context->Flush();
                const auto captured=now_us();
                if(!encoder.submit_texture(texture.Get(),sequence,sequence==0,captured))throw std::runtime_error("Generated quality encoder rejected an idle slot.");
                Encoder::Completion completion;
                if(!encoder.receive(completion)||completion.frame.sequence!=sequence||completion.frame.capture_time_us!=captured||completion.frame.keyframe!=(sequence==0))
                    throw std::runtime_error("Generated quality stream lost frame identity or inserted an unexpected IDR.");
                if(completion.average_qp>51||(!constqp&&completion.average_qp>AutoRateMaxQp))throw std::runtime_error("Generated quality stream reported invalid or excessive QP.");
                metrics.bytes+=completion.frame.data.size();metrics.qp_sum+=completion.average_qp;
                metrics.max_qp=std::max(metrics.max_qp,completion.average_qp);metrics.encode_us+=completion.total_us;
                decoder.feed(completion.frame);
                if(metrics.decoded!=sequence+1)throw std::runtime_error("Generated quality decoder buffered or lost a frame.");
            }
            encoder.finish();Encoder::Completion extra;
            if(encoder.receive(extra))throw std::runtime_error("Generated quality encoder produced an extra frame after EOS.");
            metrics.seconds=std::chrono::duration<double>(Clock::now()-began).count();
            return metrics;
        };
        const auto baseline=exercise(true),cbr=exercise(false);
        std::ostringstream text;text<<std::fixed<<std::setprecision(3);
        const auto describe=[&](const char* label,const Metrics& m){
            text<<label<<": "<<m.decoded<<'/'<<frames<<" frames, "<<m.bytes<<" bytes ("<<m.bytes*8.0/1000000.0
                <<" Mbps at nominal60FPS), RGB MAE="<<double(m.absolute[2])/m.pixels<<'/'<<double(m.absolute[1])/m.pixels<<'/'<<double(m.absolute[0])/m.pixels
                <<", RGB max="<<m.maximum[2]<<'/'<<m.maximum[1]<<'/'<<m.maximum[0]
                <<", PSNR="<<m.all_psnr()<<" dB, text/moving="<<m.text_psnr()<<" dB, chroma="<<m.chroma_psnr()
                <<" dB, frameAvgQP mean/max="<<double(m.qp_sum)/frames<<'/'<<m.max_qp
                <<", encode residence="<<double(m.encode_us)/frames/1000.0<<" ms/frame, test elapsed="<<m.seconds<<"s.\n";
        };
        describe("CQP18 reference",baseline);describe("Automatic CBR",cbr);
        // Provisional regression bounds, not a perceptual-quality certificate.
        // Five dB allows the initial four-QP policy range while flagging a much
        // larger degradation. Independent regions prevent flat background from
        // hiding damage to fine text or alternating chroma.
        const bool good=cbr.all_psnr()>=28.0&&cbr.text_psnr()>=28.0&&cbr.chroma_psnr()>=28.0&&
            cbr.all_psnr()>=baseline.all_psnr()-5.0&&cbr.text_psnr()>=baseline.text_psnr()-5.0&&cbr.chroma_psnr()>=baseline.chroma_psnr()-5.0;
        report=std::string(good?"PASS: ":"FAIL: quality regression threshold exceeded. ")+
            "1280x720 generated 14px text, colored edges, one-pixel chroma, scrolling and moving detail; same60frames per mode; single initial IDR; HEVC444 NVDEC.\n"+text.str()+
            "Automatic initial target="+std::to_string(initial_auto_bitrate(width,height,60000))+" bps. Fixed-target codec comparison, not network adaptation or a desktop throughput measurement; no visible window, desktop capture, or input. Visual review of representative real workloads remains necessary.";
        return good;
    }catch(const std::exception& error){report="FAIL: "+phase+": "+error.what();return false;}
}
bool media_color_self_test(std::string& report) {
    try {
        Cuda cuda;
        std::unique_ptr<HiveGpu,decltype(&hive_gpu_destroy)> gpu(hive_gpu_create(),hive_gpu_destroy);
        struct Buffer {Cuda& cuda;CUdeviceptr data=0;~Buffer(){if(data)cuda.free_mem(data);}};
        using Fill=CUresult CUDAAPI(CUdeviceptr,unsigned char,size_t);
        auto fill=cuda.library.get<Fill>("cuMemsetD8_v2");
        auto rounded=[](double value){return static_cast<int>(std::clamp(std::lround(value),0L,255L));};
        for(const auto [width,height]:{std::pair<uint32_t,uint32_t>{67,19},{257,33}}) {
            Buffer pixels{cuda};size_t pitch=0;
            cu(cuda.alloc_pitch(&pixels.data,&pitch,width,size_t(height)*3+1,16),"Allocate padded color test");
            cu(fill(pixels.data,0xa5,pitch*(size_t(height)*3+1)),"Fill color test guards");
            std::vector<uint8_t> source(size_t(width)*height*4),decoded(source.size()),planes(pitch*(size_t(height)*3+1));
            uint32_t random=0x27182818;
            for(auto& byte:source){random=random*1664525u+1013904223u;byte=uint8_t(random>>24);}
            if(!hive_gpu_encode_host(gpu.get(),source.data(),width,height,pixels.data,pitch))throw std::runtime_error(hive_gpu_error(gpu.get()));
            CUDA_MEMCPY2D copy{};copy.srcMemoryType=CU_MEMORYTYPE_DEVICE;copy.srcDevice=pixels.data;copy.srcPitch=pitch;
            copy.dstMemoryType=CU_MEMORYTYPE_HOST;copy.dstHost=planes.data();copy.dstPitch=pitch;copy.WidthInBytes=pitch;copy.Height=size_t(height)*3+1;
            cu(cuda.copy2d(&copy),"Read padded color planes");
            for(size_t row=0;row<copy.Height;++row)for(size_t x=row==size_t(height)*3?0:width;x<pitch;++x)
                if(planes[row*pitch+x]!=0xa5)throw std::runtime_error("PTX color kernel wrote outside the active image");
            if(!hive_gpu_decode_host(gpu.get(),pixels.data,pitch,width,height,decoded.data()))throw std::runtime_error(hive_gpu_error(gpu.get()));
            for(uint32_t y=0;y<height;++y)for(uint32_t x=0;x<width;++x) {
                const size_t at=(size_t(y)*width+x)*4;
                const double blue=source[at],green=source[at+1],red=source[at+2];
                const double luma=.2126*red+.7152*green+.0722*blue;
                const int expected[]{rounded(luma),rounded((blue-luma)/1.8556+128),rounded((red-luma)/1.5748+128)};
                for(int plane=0;plane<3;++plane)if(std::abs(int(planes[(size_t(y)+size_t(plane)*height)*pitch+x])-expected[plane])>1)
                    throw std::runtime_error("PTX forward conversion differs from CPU BT.709 reference");
                const double Y=planes[size_t(y)*pitch+x],U=int(planes[(size_t(y)+height)*pitch+x])-128,V=int(planes[(size_t(y)+height*2)*pitch+x])-128;
                const int rgb[]{rounded(Y+1.8556*U),rounded(Y-.1873242729*U-.4681242729*V),rounded(Y+1.5748*V)};
                for(int channel=0;channel<3;++channel)if(std::abs(int(decoded[at+channel])-rgb[channel])>1)
                    throw std::runtime_error("PTX reverse conversion differs from CPU BT.709 reference");
                if(decoded[at+3]!=255)throw std::runtime_error("Decoded alpha is not opaque");
            }
        }
        report="PASS: driver-only PTX 6.4 GPU kernels vs CPU BT.709 reference; random colors, 67x19 and 257x33, padded planes, untouched guards, scratch resize, opaque alpha; no CUDA runtime.";return true;
    }catch(const std::exception& error){report=std::string("FAIL: ")+error.what();return false;}
}
bool media_capture_self_test(std::string& report) {
    struct TestWindow { HWND value = nullptr; ~TestWindow() { if (value) DestroyWindow(value); } } window;
    struct DpiScope {
        DPI_AWARENESS_CONTEXT previous = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        ~DpiScope() { if (previous) SetThreadDpiAwarenessContext(previous); }
    } dpi;
    try {
        Capture capture;
        Encoder encoder(capture.width, capture.height, StreamSettings{60, 200}, capture.adapter());
        Image reconstructed;
        Decoder decoder([&](uint64_t yuv, size_t pitch, uint32_t width, uint32_t height, HiveGpu* gpu) {
            reconstructed = Image{width, height, std::vector<uint8_t>(size_t(width) * height * 4)};
            if (!hive_gpu_decode_host(gpu, yuv, pitch, width, height, reconstructed.bgra.data())) throw std::runtime_error(hive_gpu_error(gpu));
        }, capture.adapter());
        constexpr wchar_t class_name[] = L"HiveDeskCapturePatternTest";
        WNDCLASSW cls{}; cls.hInstance = GetModuleHandleW(nullptr); cls.lpszClassName = class_name;
        cls.lpfnWndProc = [](HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) -> LRESULT {
            if (message != WM_PAINT) return DefWindowProcW(hwnd, message, wparam, lparam);
            PAINTSTRUCT paint{}; HDC dc = BeginPaint(hwnd, &paint);
            RECT client{}; GetClientRect(hwnd, &client);
            const COLORREF colors[] = {RGB(30,60,220), RGB(220,60,30), RGB(60,220,30), RGB(180,180,180)};
            for (int i = 0; i < 4; ++i) {
                RECT area{(i & 1) * client.right / 2, (i / 2) * client.bottom / 2,
                    ((i & 1) + 1) * client.right / 2, (i / 2 + 1) * client.bottom / 2};
                HBRUSH brush = CreateSolidBrush(colors[i]); FillRect(dc, &area, brush); DeleteObject(brush);
            }
            EndPaint(hwnd, &paint); return 0;
        };
        if (!RegisterClassW(&cls) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) throw std::runtime_error("Cannot register capture pattern window.");
        const RECT primary = capture.bounds();
        window.value = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_TOPMOST, class_name, L"HiveDesk capture verification",
            WS_POPUP, primary.left + 32, primary.top + 32, 256, 192, nullptr, nullptr, cls.hInstance, nullptr);
        if (!window.value) throw std::runtime_error("Cannot create capture pattern window.");
        ShowWindow(window.value, SW_SHOWNOACTIVATE); UpdateWindow(window.value);
        const uint8_t colors[][3] = {{220,60,30},{30,60,220},{30,220,60},{180,180,180}};
        const auto matches = [&](const Image& image) {
            if (image.width != capture.width || image.height != capture.height || image.bgra.empty()) return false;
            for (uint32_t i = 0; i < 4; ++i) {
                const uint32_t x = 32 + (i & 1) * 128 + 64, y = 32 + (i / 2) * 96 + 48;
                if (x >= image.width || y >= image.height) return false;
                for (size_t c = 0; c < 3; ++c)
                    if (std::abs(int(image.bgra[(size_t(y) * image.width + x) * 4 + c]) - int(colors[i][c])) > 10) return false;
            }
            return true;
        };
        Image reference;
        bool captured = false;
        const auto until = Clock::now() + std::chrono::seconds(3);
        while (Clock::now() < until) {
            MSG message{};
            while (PeekMessageW(&message, window.value, 0, 0, PM_REMOVE)) { TranslateMessage(&message); DispatchMessageW(&message); }
            if (capture.next()) {
                reference = capture.readback_for_test();
                if (matches(reference)) { captured = true; break; }
            }
        }
        DestroyWindow(window.value); window.value = nullptr;
        if (!captured) throw std::runtime_error("Desktop Duplication did not capture the visible four-color test window; black/incorrect frames are not accepted.");
        const auto started = Clock::now();
        auto frame = encoder.encode_texture(capture.texture(), 0, true);
        decoder.feed(frame);
        if (!frame.keyframe || reconstructed.width != reference.width || reconstructed.height != reference.height || reconstructed.bgra.empty())
            throw std::runtime_error("Captured desktop did not round-trip with correct size and IDR.");
        if (!matches(reconstructed)) throw std::runtime_error("Captured test window colors did not survive HEVC444 roundtrip.");
        double error = 0; int maximum = 0;
        const size_t count = size_t(reference.width) * reference.height;
        for (size_t i = 0; i < count; ++i) for (size_t c = 0; c < 3; ++c) {
            const int delta = std::abs(int(reference.bgra[i * 4 + c]) - int(reconstructed.bgra[i * 4 + c]));
            error += delta; maximum = std::max(maximum, delta);
        }
        error /= double(count * 3);
        if (error > 12.) throw std::runtime_error("Captured desktop color error exceeds threshold: " + std::to_string(error));
        std::ostringstream text;
        text << "PASS: actual primary Desktop Duplication -> CUDA/D3D11 interop -> NVENC HEVC444 -> NVDEC -> CUDA RGB, "
             << reference.width << 'x' << reference.height << ", IDR=" << frame.data.size() << " bytes, RGB MAE="
             << std::fixed << std::setprecision(3) << error << ", max=" << maximum << ", encode/decode/test-readback="
             << std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started).count()
             << "ms. All four known window colors verified in both real desktop capture and decoded result; test window closed, no screenshot saved. No network/display latency claim.";
        report = text.str(); return true;
    } catch (const std::exception& error) { report = std::string("FAIL: ") + error.what(); return false; }
}

bool media_render_self_test(std::string& report) {
    struct TestWindow { HWND value = nullptr; ~TestWindow() { if (value) DestroyWindow(value); } } window;
    try {
        constexpr wchar_t class_name[] = L"HiveDeskMediaHiddenTest";
        WNDCLASSW cls{}; cls.lpfnWndProc = DefWindowProcW; cls.hInstance = GetModuleHandleW(nullptr); cls.lpszClassName = class_name;
        if (!RegisterClassW(&cls) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) throw std::runtime_error("Cannot register hidden media test window.");
        window.value = CreateWindowExW(0, class_name, L"HiveDesk hidden media test", WS_POPUP, -32000, -32000, 512, 512, nullptr, nullptr, cls.hInstance, nullptr);
        if (!window.value) throw std::runtime_error("Cannot create hidden media test window.");
        Renderer renderer(window.value);
        constexpr uint32_t width = 256, height = 192;
        Image source{width, height, std::vector<uint8_t>(size_t(width) * height * 4)};
        const uint8_t colors[][3] = {{220,60,30},{30,60,220},{30,220,60},{180,180,180}};
        for (uint32_t y = 0; y < height; ++y) for (uint32_t x = 0; x < width; ++x) {
            const auto& color = colors[(y >= height / 2 ? 2 : 0) + (x >= width / 2 ? 1 : 0)];
            auto* pixel = source.bgra.data() + (size_t(y) * width + x) * 4;
            pixel[0] = color[0]; pixel[1] = color[1]; pixel[2] = color[2]; pixel[3] = 255;
        }
        Encoder encoder(width, height, StreamSettings{60, 100});
        Image proof;
        CursorPresentation cursor;
        std::vector<CursorPresentation> presented_cursors;
        renderer.set_cursor_presented([&](const CursorPresentation& value){presented_cursors.push_back(value);});
        Decoder decoder([&](uint64_t yuv, size_t pitch, uint32_t w, uint32_t h, HiveGpu* gpu) {
            renderer.draw(yuv, pitch, w, h, gpu, &proof,nullptr,nullptr,nullptr,{},false,cursor);
        }, renderer.adapter());
        const auto verify = [&](uint32_t x, uint32_t y, const uint8_t* expected) {
            if (proof.bgra.empty() || x >= proof.width || y >= proof.height) throw std::runtime_error("No rendered proof image.");
            for (size_t c = 0; c < 3; ++c)
                if (std::abs(int(proof.bgra[(size_t(y) * proof.width + x) * 4 + c]) - int(expected[c])) > 8)
                    throw std::runtime_error("D3D11 displayed color or letterbox mismatch at " + std::to_string(x) + "," + std::to_string(y));
        };
        const uint8_t black[] = {0,0,0};
        decoder.feed(encoder.encode(source, 0, true));
        if (proof.width != 512 || proof.height != 512) throw std::runtime_error("Hidden swapchain size mismatch.");
        verify(256, 20, black); verify(256, 492, black);
        verify(128, 160, colors[0]); verify(384, 160, colors[1]); verify(128, 352, colors[2]); verify(384, 352, colors[3]);
        if (!SetWindowPos(window.value, nullptr, -32000, -32000, 640, 360, SWP_NOACTIVATE | SWP_NOZORDER)) throw std::runtime_error("Cannot resize hidden test window.");
        decoder.feed(encoder.encode(source, 1, false));
        if (proof.width != 640 || proof.height != 360) throw std::runtime_error("Resized swapchain size mismatch.");
        verify(20, 180, black); verify(620, 180, black);
        verify(200, 90, colors[0]); verify(440, 90, colors[1]); verify(200, 270, colors[2]); verify(440, 270, colors[3]);
        // Deterministic Present backpressure on the real prepared GPU picture.
        // The final static frame must retry without another encode/decode; an
        // expired or superseded picture must never be replayed afterward.
        unsigned present_calls=0;bool busy=true;
        renderer.set_present_for_test([&]{++present_calls;return busy?DXGI_ERROR_WAS_STILL_DRAWING:S_OK;});
        cursor={41,2,CursorComposition::Separate,true};
        decoder.feed(encoder.encode(source,2,false));
        if(!renderer.present_pending()||present_calls!=1)throw std::runtime_error("Busy Present did not retain the prepared final frame.");
        if(renderer.retry_present()||!renderer.present_pending()||present_calls!=2)throw std::runtime_error("Busy Present retry lost the prepared frame.");
        if(!presented_cursors.empty())throw std::runtime_error("Cursor ownership changed before successful presentation.");
        busy=false;
        if(!renderer.retry_present()||renderer.present_pending()||present_calls!=3)throw std::runtime_error("Static final frame did not present on retry without another decode.");
        if(renderer.retry_present()||present_calls!=3)throw std::runtime_error("Completed presentation replayed the same frame.");
        if(presented_cursors.size()!=1||presented_cursors.back().session!=41||presented_cursors.back().sequence!=2||presented_cursors.back().composition!=CursorComposition::Separate)
            throw std::runtime_error("Successful retry did not retain the prepared cursor metadata.");
        cursor={41,3,CursorComposition::NoSeparate,true};
        busy=true;decoder.feed(encoder.encode(source,3,false));
        const auto before_cancel=present_calls;
        if(renderer.retry_present([]{return false;})||renderer.present_pending()||present_calls!=before_cancel)
            throw std::runtime_error("Expired prepared frame was submitted on retry.");
        cursor={41,4,CursorComposition::Separate,true};decoder.feed(encoder.encode(source,4,false));
        Image latest=source;
        for(size_t pixel=0;pixel<latest.bgra.size();pixel+=4)for(size_t c=0;c<3;++c)latest.bgra[pixel+c]=colors[2][c];
        cursor={41,5,CursorComposition::NoSeparate,true};decoder.feed(encoder.encode(latest,5,false));
        verify(200,90,colors[2]);verify(440,270,colors[2]);
        busy=false;const auto before_latest=present_calls;
        if(!renderer.retry_present()||renderer.present_pending()||present_calls!=before_latest+1||renderer.retry_present())
            throw std::runtime_error("Newer prepared frame did not supersede the old pending presentation.");
        if(presented_cursors.size()!=2||presented_cursors.back().sequence!=5||presented_cursors.back().composition!=CursorComposition::NoSeparate)
            throw std::runtime_error("Cancelled or replaced image leaked its cursor ownership.");
        // Exercise the queue/renderer boundary: a compressed arrival has not
        // produced replacement pixels, so it must not starve a prepared frame.
        ViewerQueue pending_frames;
        EncodedFrame metadata;metadata.session=1;metadata.sequence=0;metadata.capture_time_us=1000000;metadata.keyframe=true;metadata.data={1};
        if(!pending_frames.push(metadata,2000000))throw std::runtime_error("Prepared-frame test could not establish an epoch.");
        auto prepared=pending_frames.pop(2000000);
        cursor={41,6,CursorComposition::Separate,true};busy=true;decoder.feed(encoder.encode(source,6,false));
        metadata.sequence=1;metadata.capture_time_us+=5000;metadata.keyframe=false;
        if(!prepared||!pending_frames.push(metadata,2005000)||pending_frames.presentable(*prepared,2005000))
            throw std::runtime_error("Prepared-frame test did not queue the newer compressed picture.");
        busy=false;const auto before_arrival_retry=present_calls;
        if(!renderer.retry_present([&]{return pending_frames.fresh(*prepared,2005000);})||renderer.present_pending()||
            present_calls!=before_arrival_retry+1||renderer.retry_present())
            throw std::runtime_error("Fresh prepared presentation starved behind a compressed arrival.");
        cursor={41,7,CursorComposition::NoSeparate,true};busy=true;decoder.feed(encoder.encode(source,7,false));
        metadata.sequence=2;metadata.capture_time_us+=5000;metadata.keyframe=true;
        if(!pending_frames.push(metadata,2010000))throw std::runtime_error("Prepared-frame test replacement IDR was rejected.");
        const auto before_epoch_retry=present_calls;
        if(renderer.retry_present([&]{return pending_frames.fresh(*prepared,2010000);})||renderer.present_pending()||present_calls!=before_epoch_retry)
            throw std::runtime_error("Prepared presentation crossed a replacement IDR epoch.");
        if(presented_cursors.size()!=3||presented_cursors.back().sequence!=6||presented_cursors.back().composition!=CursorComposition::Separate)
            throw std::runtime_error("Replacement IDR epoch changed displayed cursor ownership.");
        renderer.set_present_for_test([]{return DXGI_STATUS_OCCLUDED;});
        cursor={42,0,CursorComposition::NoSeparate,true};decoder.feed(encoder.encode(source,8,false));
        if(presented_cursors.size()!=3||!renderer.present_pending())throw std::runtime_error("Occluded frame changed cursor ownership.");
        renderer.set_present_for_test([]{return S_OK;});
        if(!renderer.retry_present()||presented_cursors.size()!=4||presented_cursors.back().session!=42||presented_cursors.back().sequence!=0)
            throw std::runtime_error("First visible frame lost its session cursor ownership.");
        renderer.set_present_for_test({});
        report = "PASS: actual NVENC444 -> NVDEC -> CUDA/D3D11 interop -> HWND swapchain; GPU backbuffer readback verifies four color quadrants, orientation, letterbox at 512x512 and pillarbox after resize to 640x360. Injected busy Present verifies final-static-frame retry without another decode, freshness cancellation, newest prepared frame replacement without replay, compressed-arrival progress, and replacement-IDR epoch rejection. Cursor metadata follows only successful presentation, including replacement, cancellation, occlusion and a new session. Hidden window; no desktop interaction or physical-scanout claim.";
        return true;
    } catch (const std::exception& error) { report = std::string("FAIL: ") + error.what(); return false; }
}
bool media_throughput_self_test(std::string& report) {
    struct TestWindow { HWND value = nullptr; ~TestWindow() { if (value) DestroyWindow(value); } } window;
    try {
        constexpr wchar_t class_name[] = L"HiveDeskMediaThroughputTest";
        WNDCLASSW cls{}; cls.lpfnWndProc = DefWindowProcW; cls.hInstance = GetModuleHandleW(nullptr); cls.lpszClassName = class_name;
        if (!RegisterClassW(&cls) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) throw std::runtime_error("Cannot register throughput window.");
        window.value = CreateWindowExW(0, class_name, L"HiveDesk hidden throughput test", WS_POPUP, -32000, -32000, 1280, 720, nullptr, nullptr, cls.hInstance, nullptr);
        if (!window.value) throw std::runtime_error("Cannot create throughput window.");
        Renderer renderer(window.value);
        constexpr uint32_t width = 2560, height = 1440;
        Image source{width, height, std::vector<uint8_t>(size_t(width) * height * 4)};
        for (uint32_t y = 0; y < height; ++y) for (uint32_t x = 0; x < width; ++x) {
            auto* pixel = source.bgra.data() + (size_t(y) * width + x) * 4;
            const bool grid = (x % 64 < 2) || (y % 32 < 2);
            pixel[0] = grid ? 250 : static_cast<uint8_t>((x / 8 + y / 8) % 256);
            pixel[1] = grid ? 40 : static_cast<uint8_t>((x / 16) % 256);
            pixel[2] = grid ? 180 : static_cast<uint8_t>((y / 8) % 256); pixel[3] = 255;
        }
        auto texture = renderer.upload_for_test(source);
        Encoder encoder(width, height, StreamSettings{240, 100}, renderer.adapter());
        uint32_t displayed = 0;
        Decoder decoder([&](uint64_t yuv, size_t pitch, uint32_t w, uint32_t h, HiveGpu* gpu) {
            renderer.draw(yuv, pitch, w, h, gpu); ++displayed;
        }, renderer.adapter());
        constexpr uint32_t warmup = 8, measured = 120;
        for (uint32_t i = 0; i < warmup; ++i) decoder.feed(encoder.encode_texture(texture.Get(), i, i == 0));
        uint64_t total_bytes = 0;
        const auto started = Clock::now();
        for (uint32_t i = 0; i < measured; ++i) {
            auto frame = encoder.encode_texture(texture.Get(), warmup + i, false);
            total_bytes += frame.data.size(); decoder.feed(frame);
        }
        const double seconds = std::chrono::duration<double>(Clock::now() - started).count();
        if (displayed != warmup + measured) throw std::runtime_error("Throughput test lost decoded/rendered frames.");
        std::ostringstream text;
        text << "PASS: " << measured << '/' << measured << " measured frames, 2560x1440 HEVC444 full GPU pixel path, "
             << std::fixed << std::setprecision(1) << measured / seconds << " frames/s (" << seconds * 1000 / measured
             << "ms mean serialized processing/frame), " << total_bytes * 8 / seconds / 1000000 << " Mbps produced. "
             << "Fixed synthetic GPU texture, eight warmup frames, hidden 1280x720 render window, no acquisition/network/input/display-scanout; NOT remote input latency or general content performance.";
        report = text.str(); return true;
    } catch (const std::exception& error) { report = std::string("FAIL: ") + error.what(); return false; }
}
bool media_pipeline_self_test(std::string& report) {
    struct TestWindow { HWND value = nullptr; ~TestWindow() { if (value) DestroyWindow(value); } } window;
    std::string phase = "pipeline window initialization";
    try {
        constexpr wchar_t class_name[] = L"HiveDeskMediaPipelineTest";
        WNDCLASSW cls{}; cls.lpfnWndProc = DefWindowProcW; cls.hInstance = GetModuleHandleW(nullptr); cls.lpszClassName = class_name;
        if (!RegisterClassW(&cls) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) throw std::runtime_error("Cannot register pipeline test window.");
        window.value = CreateWindowExW(0, class_name, L"HiveDesk hidden pipeline test", WS_POPUP, -32000, -32000, 256, 192, nullptr, nullptr, cls.hInstance, nullptr);
        if (!window.value) throw std::runtime_error("Cannot create pipeline test window.");
        Renderer renderer(window.value);
        const uint8_t colors[][3] = {{220,60,30},{30,60,220},{30,220,60},{180,180,180},{60,180,220},{220,180,60},{160,50,180},{50,160,180}};
        constexpr uint32_t width = 256, height = 192, count = 24;
        Image source{width, height, std::vector<uint8_t>(size_t(width) * height * 4)};
        auto texture = renderer.upload_for_test(source);
        ComPtr<ID3D11Device> device; texture->GetDevice(&device);
        ComPtr<ID3D11DeviceContext> context; device->GetImmediateContext(&context);
        bool native_async = false;
        uint32_t rate_changes = 0;
        const auto exercise = [&](bool forced_sync) {
            const std::string mode = forced_sync ? "forced sync" : "native mode";
            phase = mode + " frame rotation";
            std::mutex mutex; std::condition_variable cv;
            uint32_t permits = 0, received = 0; bool abort = false;
            std::exception_ptr failure;
            uint64_t timestamps[count]{};
            {
            Encoder encoder(width, height, StreamSettings{0,0,240000,1}, renderer.adapter(), forced_sync);
            const auto original_bitrate = encoder.bitrate_bps();
            if (!encoder.automatic_rate() || original_bitrate != initial_auto_bitrate(width,height,240000))
                throw std::runtime_error("Automatic pipeline did not select its CBR rate policy.");
            if (!forced_sync) native_async = encoder.asynchronous();
            if (forced_sync && encoder.asynchronous()) throw std::runtime_error("Pipeline fallback test did not select synchronous NVENC.");
            std::thread output([&] {
                try {
                    uint32_t expected = 0, decoded = 0;
                    Decoder decoder([&](uint64_t yuv, size_t pitch, uint32_t w, uint32_t h, HiveGpu* gpu) {
                        if (w != width || h != height) throw std::runtime_error("Pipeline decoded dimensions changed.");
                        Image proof{w,h,std::vector<uint8_t>(size_t(w)*h*4)};
                        if (!hive_gpu_decode_host(gpu,yuv,pitch,w,h,proof.bgra.data())) throw std::runtime_error(hive_gpu_error(gpu));
                        for (uint32_t quadrant = 0; quadrant < 4; ++quadrant) {
                            const uint32_t x = (quadrant & 1) * width / 2 + width / 4;
                            const uint32_t y = (quadrant / 2) * height / 2 + height / 4;
                            const auto& color = colors[(expected + quadrant) % 8];
                            for (size_t channel = 0; channel < 3; ++channel)
                                if (std::abs(int(proof.bgra[(size_t(y)*width+x)*4+channel])-int(color[channel])) > 10)
                                    throw std::runtime_error("Pipeline reused an in-flight surface or reordered frame colors.");
                        }
                        ++decoded;
                    }, renderer.adapter());
                    for (; expected < count; ++expected) {
                        {
                            std::unique_lock lock(mutex); cv.wait(lock,[&]{return abort || permits != 0;});
                            if (abort) return;
                            --permits;
                        }
                        Encoder::Completion completion;
                        if (!encoder.receive(completion)) throw std::runtime_error("Pipeline ended before all submitted frames drained.");
                        if (completion.frame.sequence != expected || completion.frame.capture_time_us != timestamps[expected] || completion.frame.data.empty())
                            throw std::runtime_error("Pipeline changed sequence, capture timestamp or bitstream ownership.");
                        if(completion.frame.cursor_composition!=static_cast<CursorComposition>(expected%3))
                            throw std::runtime_error("Pipeline reused cursor metadata from a different encoding slot.");
                        if (completion.submitted_us < timestamps[expected] || completion.completed_us < completion.submitted_us || completion.total_us < completion.color_us ||
                            completion.frame.source_ready_us!=completion.completed_us||completion.frame.source_ready_us<completion.frame.capture_time_us)
                            throw std::runtime_error("Pipeline completion timing is not monotonic.");
                        if ((expected == 0 || expected == 12) && !completion.frame.keyframe)
                            throw std::runtime_error("Pipeline did not honor requested IDR.");
                        if (expected != 0 && expected != 12 && completion.frame.keyframe)
                            throw std::runtime_error("Bitrate/frame-rate adjustment unexpectedly inserted an IDR.");
                        if (completion.average_qp > AutoRateMaxQp)
                            throw std::runtime_error("CBR completion QP is missing or exceeds the configured quality ceiling.");
                        decoder.feed(completion.frame);
                        if (decoded != expected + 1) throw std::runtime_error("Pipeline decoder buffered or lost an output frame.");
                        { std::lock_guard lock(mutex); ++received; }
                        cv.notify_all();
                    }
                    Encoder::Completion extra;
                    if (encoder.receive(extra)) throw std::runtime_error("Pipeline produced an unsubmitted extra frame after EOS.");
                } catch (...) {
                    { std::lock_guard lock(mutex); failure = std::current_exception(); }
                    cv.notify_all();
                }
            });
            try {
                for (uint32_t batch = 0; batch < count; batch += 2) {
                    if (encoder.dynamic_rate() && (batch == 4 || batch == 8 || batch == 16)) {
                        phase = mode + " live CBR reconfiguration";
                        const uint32_t changed_bitrate = batch == 4 ? original_bitrate/2 : batch == 8 ? original_bitrate*3/2 : original_bitrate;
                        const uint32_t changed_rate = batch == 4 ? 119880 : batch == 8 ? 59940 : 240000;
                        if (!encoder.reconfigure(changed_bitrate,changed_rate) || encoder.bitrate_bps()!=changed_bitrate || encoder.nominal_millihz()!=changed_rate)
                            throw std::runtime_error("Idle encoder rejected dynamic bitrate/frame-rate adjustment.");
                        ++rate_changes;
                    }
                    for (uint32_t sequence = batch; sequence < batch + 2; ++sequence) {
                        for (uint32_t y = 0; y < height; ++y) for (uint32_t x = 0; x < width; ++x) {
                            const uint32_t quadrant = (y >= height/2 ? 2 : 0) + (x >= width/2 ? 1 : 0);
                            const auto& color = colors[(sequence + quadrant) % 8];
                            auto* pixel = source.bgra.data() + (size_t(y)*width+x)*4;
                            pixel[0]=color[0];pixel[1]=color[1];pixel[2]=color[2];pixel[3]=255;
                        }
                        context->UpdateSubresource(texture.Get(),0,nullptr,source.bgra.data(),width*4,0); context->Flush();
                        timestamps[sequence] = now_us();
                        bool completion_queued = false;
                        if (!encoder.submit_texture(texture.Get(),sequence,sequence==0 || sequence==12,timestamps[sequence],&completion_queued,static_cast<CursorComposition>(sequence%3)) || !completion_queued)
                            throw std::runtime_error("Pipeline rejected a free slot.");
                    }
                    bool rejected_completion = true;
                    const bool overcommitted = encoder.submit_texture(texture.Get(),count+1,false,now_us(),&rejected_completion);
                    if (encoder.in_flight() != Encoder::max_in_flight || encoder.ready() || overcommitted || rejected_completion)
                        throw std::runtime_error("Pipeline allowed more than two in-flight frames.");
                    if (encoder.reconfigure(original_bitrate,240000))
                        throw std::runtime_error("Encoder reconfigured while outputs were still in flight.");
                    // Finish with both outputs still owned by NVENC. The output
                    // thread then drains these in order before its final false.
                    if (batch + 2 == count) encoder.finish();
                    { std::lock_guard lock(mutex); permits = 2; }
                    cv.notify_all();
                    std::unique_lock lock(mutex);
                    if (!cv.wait_for(lock,std::chrono::seconds(10),[&]{return failure || received == batch+2;}))
                        throw std::runtime_error("Pipeline hardware verification timed out.");
                    if (failure) std::rethrow_exception(failure);
                }
                output.join();
                if (failure) std::rethrow_exception(failure);
                if (encoder.in_flight() || encoder.ready()) throw std::runtime_error("Finished pipeline retained a slot or accepted more work.");
            } catch (...) {
                { std::lock_guard lock(mutex); abort = true; }
                cv.notify_all();
                try { encoder.finish(); } catch (...) {}
                if (output.joinable()) output.join();
                throw;
            }
            } // Unregister the shared D3D texture before another encoder uses it.
            // Empty shutdown and construction/destruction without a worker are
            // separate ownership cases from the draining session above.
            phase = mode + " idle shutdown";
            { Encoder idle(width,height,StreamSettings{0,0,240000,2},renderer.adapter(),forced_sync); idle.finish(); }
            {
                phase = mode + " cancelled wrapped-slot shutdown";
                Encoder cancelled(width,height,StreamSettings{0,0,240000,2},renderer.adapter(),forced_sync);
                for (uint32_t i=0;i<3;++i) cancelled.encode_texture(texture.Get(),i,i==0);
                if (!cancelled.submit_texture(texture.Get(),3,false,now_us()) || !cancelled.submit_texture(texture.Get(),4,false,now_us()))
                    throw std::runtime_error("Cannot prepare cancelled pipeline verification.");
                // Simulate the consumer exiting because a downstream callback
                // failed. Destructor must drain wrapped slots3,0 in that order.
            }
        };
        exercise(false); exercise(true);

        // Comparable encode-only observations within this build. Fixed content,
        // no acquisition/decode/network and no assertion about absolute speed.
        phase = "benchmark texture creation";
        constexpr uint32_t benchmark_width=2560, benchmark_height=1440, warmup=8, measured=120;
        Image benchmark{benchmark_width,benchmark_height,std::vector<uint8_t>(size_t(benchmark_width)*benchmark_height*4)};
        for (uint32_t y=0;y<benchmark_height;++y) for (uint32_t x=0;x<benchmark_width;++x) {
            auto* pixel=benchmark.bgra.data()+(size_t(y)*benchmark_width+x)*4;
            const bool grid=x%64<2 || y%32<2;
            pixel[0]=grid?250:uint8_t((x/8+y/8)%256);pixel[1]=grid?40:uint8_t((x/16)%256);
            pixel[2]=grid?180:uint8_t((y/8)%256);pixel[3]=255;
        }
        auto fixed=renderer.upload_for_test(benchmark);
        const auto benchmark_mode=[&](bool parallel,bool forced_sync) {
            phase = forced_sync ? "forced-sync pipeline benchmark" : parallel ? "native pipeline benchmark" : "serialized benchmark";
            std::mutex mutex; std::condition_variable cv;
            uint32_t received=0;std::exception_ptr failure;
            Encoder encoder(benchmark_width,benchmark_height,StreamSettings{0,0,240000,3,true},renderer.adapter(),forced_sync);
            for (uint32_t i=0;i<warmup;++i) encoder.encode_texture(fixed.Get(),i,i==0);
            const auto started=Clock::now();
            if (!parallel) {
                for (uint32_t i=0;i<measured;++i) encoder.encode_texture(fixed.Get(),warmup+i,false);
            } else {
                std::thread output([&] {
                    try {
                        Encoder::Completion completion;
                        while (encoder.receive(completion)) {
                            if (completion.frame.sequence != warmup+received) throw std::runtime_error("Pipeline benchmark reordered its output.");
                            { std::lock_guard lock(mutex); ++received; }
                            cv.notify_all();
                        }
                    } catch (...) {
                        { std::lock_guard lock(mutex); failure=std::current_exception(); }
                        cv.notify_all();
                    }
                });
                try {
                    for (uint32_t i=0;i<measured;++i) {
                        std::unique_lock lock(mutex);
                        if (!cv.wait_for(lock,std::chrono::seconds(10),[&]{return failure || encoder.ready();}))
                            throw std::runtime_error("Pipeline benchmark waited too long for a free slot.");
                        if (failure) std::rethrow_exception(failure);
                        lock.unlock();
                        if (!encoder.submit_texture(fixed.Get(),warmup+i,false,now_us())) throw std::runtime_error("Pipeline benchmark lost its reserved slot.");
                    }
                    encoder.finish(); output.join();
                    if (failure) std::rethrow_exception(failure);
                    if (received != measured) throw std::runtime_error("Pipeline benchmark lost a frame.");
                } catch (...) {
                    try { encoder.finish(); } catch (...) {}
                    if (output.joinable()) output.join();
                    throw;
                }
            }
            return measured/std::chrono::duration<double>(Clock::now()-started).count();
        };
        const double serial=benchmark_mode(false,false), native=benchmark_mode(true,false), fallback=benchmark_mode(true,true);
        std::ostringstream text;
        text << "PASS: 4 allocated IO pairs / max2 in flight; third submission rejected; "
             << count << " changing-color frames each in native " << (native_async?"WDDM async":"sync fallback")
             << " and forced sync modes; ordered output, capture timestamps, midstream IDR, NVDEC 444 colors, slot rotation, EOS with2pending, cancelled wrapped slots and idle shutdown verified. "
             << "Automatic CBR QP<=22; " << rate_changes << " idle bitrate/fractional-frame-rate adjustments without implicit IDRs; in-flight adjustment refused. "
             << "2560x1440 fixed-texture CQP18 encode-only observations: serialized=" << std::fixed << std::setprecision(1) << serial
             << " FPS, native pipeline=" << native << " FPS, forced-sync pipeline=" << fallback
             << " FPS (120 measured,8warmup each; no capture/network/decode/display latency claim). Hidden window only.";
        report=text.str();return true;
    } catch (const std::exception& error) {report=std::string("FAIL: ")+phase+": "+error.what();return false;}
}
#include "media_motion_test.inl"
#include "media_async_test.inl"
} // namespace hive
