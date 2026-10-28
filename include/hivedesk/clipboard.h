#pragma once
#include <windows.h>
#include <objidl.h>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace hive {
constexpr uint32_t ClipboardChunk = 256*1024;
constexpr uint64_t ClipboardImageLimit = 256ull*1024*1024;
enum class ClipboardFormat : uint32_t { Dib=1, DibV5=2, Png=3 };
struct ClipboardBlob { ClipboardFormat format{}; uint64_t size=0; };
struct ClipboardFile { std::wstring name; uint64_t size=0,modified=0; bool directory=false; };
struct ClipboardOffer {
    uint64_t id=0;
    std::wstring text;
    std::vector<ClipboardBlob> images;
    std::vector<ClipboardFile> files;
};
// Items 0..images.size()-1 are image bytes, remaining items are file streams.
// Readers return exactly the requested range or throw; no partial success.
struct ClipboardContent {
    ClipboardOffer offer;
    // Local-only provenance. Never sent over the wire or used as a file path.
    std::string upstream;
    uint64_t connection=0;
    std::function<std::vector<uint8_t>(uint32_t,uint64_t,uint32_t)> read;
};
using Clipboard = std::shared_ptr<const ClipboardContent>;
Clipboard text_clipboard(const std::wstring&);
bool valid_clipboard_offer(const ClipboardOffer&);
std::vector<uint8_t> encode_clipboard_offer(const ClipboardOffer&);
bool decode_clipboard_offer(const std::vector<uint8_t>&,ClipboardOffer&);
// Native OLE adapter. FileContents is a seekable, free-threaded IStream; no
// temporary-file staging and no source deletion, including cut/paste sources.
IDataObject* clipboard_data_object(Clipboard);
Clipboard capture_clipboard(HWND);

// Own STA and listener: delayed rendering, file enumeration and retries never
// run on the viewer, input hooks or the UI message pump.
class ClipboardDesktop {
public:
    ClipboardDesktop();~ClipboardDesktop();
    bool start(std::function<void(Clipboard)>,std::string& error);
    void stop();
    Clipboard cached()const;
    bool busy()const;
    void apply(Clipboard);
    void refresh();
private: struct Impl;std::unique_ptr<Impl> impl_;
};
class Transport;
// Transport-bound grants expose only the current advertised objects to each
// peer; request IDs, offer IDs and connection tokens fence late responses.
class ClipboardTransfer {
public:
    explicit ClipboardTransfer(Transport&);~ClipboardTransfer();
    bool send(const std::string&,Clipboard);
    Clipboard receive(const std::string&,const ClipboardOffer&);
    void packet(const std::string&,const std::vector<uint8_t>&);
    bool busy()const;
    void stop();
private: struct Impl;std::shared_ptr<Impl> impl_;
};
}
