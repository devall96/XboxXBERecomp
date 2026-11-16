#include "gpu/texturedumper.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#include "app/app.h"
#include "core/log.h"
#include "gpu/shaders.h"

using Microsoft::WRL::ComPtr;

namespace gpu::texturedumper {

namespace {

namespace fs = std::filesystem;

constexpr size_t kKeyDigits = 16;
constexpr uint32_t kMaxSide = 16384;  // D3D12's 2D texture limit
constexpr uint32_t kDumpDds = 1;      // texture_dump_format
constexpr size_t kPaletteBytes = 256 * 4;
constexpr auto kSettle = std::chrono::milliseconds(500);  // files still being copied in
constexpr uint32_t kWatchRetry = 120;  // presents between tries while the pack folder is missing
constexpr DWORD kPackChanges = FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME |
                               FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_SIZE;

using Clock = std::chrono::steady_clock;

// the settings as Update latched them, the render thread's
struct Latched {
    bool replacing = false;
    bool dumping = false;
    uint32_t format = 0;
    bool ui_upscale = false;
    bool operator==(const Latched&) const = default;
};

struct Pack {
    Latched set;
    uint32_t generation = 1;
    fs::path pack_dir;
    fs::path dump_dir;
    std::unordered_map<uint64_t, fs::path> files;  // by the key in their names; written by Update alone
    HANDLE watch = INVALID_HANDLE_VALUE;           // changes under pack_dir
    uint32_t watch_tries = 0;
    std::optional<Clock::time_point> changed;      // the last change seen, not listed yet
    std::once_flag worker;
    std::mutex lock;                               // everything below
    uint32_t revision = 0;                         // a load started before the pack was listed again is dropped
    std::condition_variable wake;
    std::deque<std::function<void()>> jobs;
    std::unordered_set<uint64_t> asked;
    std::unordered_map<uint64_t, std::optional<Image>> loaded;  // empty: the file did not load
    std::unordered_set<uint64_t> dumped;
    std::unordered_map<uint64_t, bool> edited;  // keys Edit makes, and whether as 2D, the render thread's
};
std::atomic<bool> g_refresh{false};

Pack g;

// Worker

void Work() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    for (;;) {
        std::function<void()> job;
        {
            std::unique_lock l(g.lock);
            g.wake.wait(l, [] { return !g.jobs.empty(); });
            job = std::move(g.jobs.front());
            g.jobs.pop_front();
        }
        job();
    }
}

void Post(std::function<void()> job) {
    std::call_once(g.worker, [] { std::thread(Work).detach(); });
    {
        std::lock_guard l(g.lock);
        g.jobs.push_back(std::move(job));
    }
    g.wake.notify_one();
}

// Formats

DXGI_FORMAT UploadFormat(Host h) {
    switch (h) {
        case Host::BGRA8: return DXGI_FORMAT_B8G8R8A8_UNORM;
        case Host::BC1: return DXGI_FORMAT_BC1_UNORM;
        case Host::BC2: return DXGI_FORMAT_BC2_UNORM;
        case Host::BC3: return DXGI_FORMAT_BC3_UNORM;
        default: return DXGI_FORMAT_UNKNOWN;
    }
}

bool Uncompressed(DXGI_FORMAT f) { return f == DXGI_FORMAT_B8G8R8A8_UNORM || f == DXGI_FORMAT_R8G8B8A8_UNORM; }

struct LevelShape {
    uint32_t pitch;
    uint32_t rows;
};

// pitch 0: a format packs cannot use
LevelShape Shape(DXGI_FORMAT f, uint32_t w, uint32_t h) {
    uint32_t blocks_x = std::max(1u, (w + 3) / 4);
    uint32_t blocks_y = std::max(1u, (h + 3) / 4);
    switch (f) {
        case DXGI_FORMAT_BC1_UNORM: return {blocks_x * 8, blocks_y};
        case DXGI_FORMAT_BC2_UNORM:
        case DXGI_FORMAT_BC3_UNORM:
        case DXGI_FORMAT_BC7_UNORM: return {blocks_x * 16, blocks_y};
        case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_UNORM: return {w * 4, h};
        default: return {0, 0};
    }
}

uint32_t FullChain(uint32_t w, uint32_t h) {
    uint32_t n = 1;
    while ((std::max(w, h) >> n) > 0) n++;
    return n;
}

// Names

// the 16 hex digit part of a name such as 256x256_0123456789abcdef_DXT1, 0 without one
uint64_t KeyInName(const std::wstring& stem) {
    size_t start = 0;
    while (start < stem.size()) {
        size_t end = std::min(stem.find(L'_', start), stem.size());
        std::wstring part = stem.substr(start, end - start);
        if (part.size() == kKeyDigits && part.find_first_not_of(L"0123456789abcdefABCDEF") == std::wstring::npos) {
            return std::wcstoull(part.c_str(), nullptr, 16);
        }
        start = end + 1;
    }
    return 0;
}

std::string DumpName(uint64_t key, const TextureDesc& d, const char* extension) {
    char name[96];
    std::snprintf(name, sizeof(name), "%ux%u_%016llx_%s.%s", d.width, d.height, static_cast<unsigned long long>(key),
                  Format(d.format).name, extension);
    return name;
}

std::string Utf8(const fs::path& p) {
    std::u8string s = p.u8string();
    return std::string(s.begin(), s.end());
}

void ListPack(const fs::path& dir) {
    g.files.clear();
    std::error_code ec;
    fs::recursive_directory_iterator it(dir, ec);
    for (; !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        const fs::path& p = it->path();
        bool image = _wcsicmp(p.extension().c_str(), L".dds") == 0 || _wcsicmp(p.extension().c_str(), L".png") == 0;
        uint64_t key = image ? KeyInName(p.stem().wstring()) : 0;
        if (key) g.files.emplace(key, p);
    }
    LOGR("texture pack: %zu images in %s", g.files.size(), Utf8(dir).c_str());
}

// DDS files

constexpr uint32_t FourCC(const char (&s)[5]) {
    return uint32_t(uint8_t(s[0])) | uint32_t(uint8_t(s[1])) << 8 | uint32_t(uint8_t(s[2])) << 16 |
           uint32_t(uint8_t(s[3])) << 24;
}

// "DDS " then DDS_HEADER, byte offsets from the file start; DDS_HEADER_DXT10 follows when the FourCC is DX10
constexpr uint32_t kDdsMagic = FourCC("DDS ");
constexpr size_t kDdsSize = 4;
constexpr size_t kDdsFlags = 8;
constexpr size_t kDdsHeight = 12;
constexpr size_t kDdsWidth = 16;
constexpr size_t kDdsPitch = 20;
constexpr size_t kDdsMipCount = 28;
constexpr size_t kDdsPixelSize = 76;
constexpr size_t kDdsPixelFlags = 80;
constexpr size_t kDdsFourCC = 84;
constexpr size_t kDdsBitCount = 88;
constexpr size_t kDdsRedMask = 92;
constexpr size_t kDdsGreenMask = 96;
constexpr size_t kDdsBlueMask = 100;
constexpr size_t kDdsAlphaMask = 104;
constexpr size_t kDdsCaps = 108;
constexpr size_t kDdsHeaderBytes = 128;
constexpr size_t kDx10Format = 128;
constexpr size_t kDx10HeaderBytes = 148;

// DDS_HEADER field values
constexpr uint32_t kDdsHeaderSize = 124;
constexpr uint32_t kDdsPixelFormatSize = 32;
constexpr uint32_t kDdsdRequired = 0x1007;  // caps, height, width, pixel format
constexpr uint32_t kDdsdPitch = 0x8;
constexpr uint32_t kDdsdMipCount = 0x20000;
constexpr uint32_t kDdsdLinearSize = 0x80000;
constexpr uint32_t kDdpfAlphaPixels = 0x1;
constexpr uint32_t kDdpfFourCC = 0x4;
constexpr uint32_t kDdpfRgb = 0x40;
constexpr uint32_t kCapsComplex = 0x8;
constexpr uint32_t kCapsTexture = 0x1000;
constexpr uint32_t kCapsMipmap = 0x400000;
constexpr uint32_t kRedOfBgra = 0x00FF0000;
constexpr uint32_t kGreenMask = 0x0000FF00;
constexpr uint32_t kBlueOfBgra = 0x000000FF;
constexpr uint32_t kRedOfRgba = 0x000000FF;
constexpr uint32_t kAlphaMask = 0xFF000000;

uint32_t Read32(const std::vector<uint8_t>& b, size_t at) {
    uint32_t v;
    std::memcpy(&v, &b[at], 4);
    return v;
}

void Write32(std::vector<uint8_t>& b, size_t at, uint32_t v) { std::memcpy(&b[at], &v, 4); }

// data: where level 0 starts
DXGI_FORMAT DdsFormat(const std::vector<uint8_t>& f, size_t& data) {
    data = kDdsHeaderBytes;
    uint32_t flags = Read32(f, kDdsPixelFlags);
    uint32_t cc = Read32(f, kDdsFourCC);
    if (flags & kDdpfFourCC) {
        if (cc == FourCC("DXT1")) return DXGI_FORMAT_BC1_UNORM;
        if (cc == FourCC("DXT3")) return DXGI_FORMAT_BC2_UNORM;
        if (cc == FourCC("DXT5")) return DXGI_FORMAT_BC3_UNORM;
        if (cc != FourCC("DX10") || f.size() < kDx10HeaderBytes) return DXGI_FORMAT_UNKNOWN;
        data = kDx10HeaderBytes;
        auto dx10 = DXGI_FORMAT(Read32(f, kDx10Format));
        return Shape(dx10, 1, 1).pitch ? dx10 : DXGI_FORMAT_UNKNOWN;
    }
    if (!(flags & kDdpfRgb) || Read32(f, kDdsBitCount) != 32) return DXGI_FORMAT_UNKNOWN;
    uint32_t red = Read32(f, kDdsRedMask);
    if (red == kRedOfBgra) return DXGI_FORMAT_B8G8R8A8_UNORM;
    return red == kRedOfRgba ? DXGI_FORMAT_R8G8B8A8_UNORM : DXGI_FORMAT_UNKNOWN;
}

// X8R8G8B8 and the like: alpha bytes left undefined
void MakeOpaque(Image& img) {
    for (Level& l : img.levels) {
        for (size_t i = 3; i < l.px.size(); i += 4) l.px[i] = 0xFF;
    }
}

bool ReadDds(const fs::path& path, Image& img) {
    std::ifstream in(path, std::ios::binary);
    std::vector<uint8_t> f{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    if (f.size() < kDdsHeaderBytes || Read32(f, 0) != kDdsMagic) return false;
    size_t at = 0;
    img.format = DdsFormat(f, at);
    img.width = Read32(f, kDdsWidth);
    img.height = Read32(f, kDdsHeight);
    if (img.format == DXGI_FORMAT_UNKNOWN || !img.width || !img.height) return false;
    if (img.width > kMaxSide || img.height > kMaxSide) return false;
    uint32_t mips = Read32(f, kDdsFlags) & kDdsdMipCount ? Read32(f, kDdsMipCount) : 1;
    uint32_t levels = std::clamp(mips, 1u, FullChain(img.width, img.height));
    for (uint32_t l = 0; l < levels; l++) {
        LevelShape s = Shape(img.format, std::max(img.width >> l, 1u), std::max(img.height >> l, 1u));
        size_t bytes = size_t(s.pitch) * s.rows;
        if (at + bytes > f.size()) break;  // short file: the levels it holds
        img.levels.push_back({std::vector<uint8_t>(f.begin() + at, f.begin() + at + bytes), s.pitch});
        at += bytes;
    }
    uint32_t flags = Read32(f, kDdsPixelFlags);
    if (!(flags & kDdpfFourCC) && !(flags & kDdpfAlphaPixels)) MakeOpaque(img);
    return !img.levels.empty();
}

std::vector<uint8_t> DdsHeader(const Image& img) {
    std::vector<uint8_t> h(kDdsHeaderBytes);
    bool bc = !Uncompressed(img.format);
    uint32_t levels = uint32_t(img.levels.size());
    const Level& top = img.levels[0];
    Write32(h, 0, kDdsMagic);
    Write32(h, kDdsSize, kDdsHeaderSize);
    Write32(h, kDdsFlags, kDdsdRequired | kDdsdMipCount | (bc ? kDdsdLinearSize : kDdsdPitch));
    Write32(h, kDdsHeight, img.height);
    Write32(h, kDdsWidth, img.width);
    Write32(h, kDdsPitch, bc ? uint32_t(top.px.size()) : top.pitch);
    Write32(h, kDdsMipCount, levels);
    Write32(h, kDdsPixelSize, kDdsPixelFormatSize);
    Write32(h, kDdsCaps, kCapsTexture | (levels > 1 ? kCapsComplex | kCapsMipmap : 0));
    if (bc) {
        uint32_t cc = FourCC("DXT1");
        if (img.format == DXGI_FORMAT_BC2_UNORM) cc = FourCC("DXT3");
        if (img.format == DXGI_FORMAT_BC3_UNORM) cc = FourCC("DXT5");
        Write32(h, kDdsPixelFlags, kDdpfFourCC);
        Write32(h, kDdsFourCC, cc);
        return h;
    }
    Write32(h, kDdsPixelFlags, kDdpfRgb | kDdpfAlphaPixels);
    Write32(h, kDdsBitCount, 32);
    Write32(h, kDdsRedMask, kRedOfBgra);
    Write32(h, kDdsGreenMask, kGreenMask);
    Write32(h, kDdsBlueMask, kBlueOfBgra);
    Write32(h, kDdsAlphaMask, kAlphaMask);
    return h;
}

void WriteDds(const fs::path& path, const Image& img) {
    std::error_code ec;
    if (fs::exists(path, ec)) return;
    std::ofstream out(path, std::ios::binary);
    std::vector<uint8_t> header = DdsHeader(img);
    out.write(reinterpret_cast<const char*>(header.data()), std::streamsize(header.size()));
    for (const Level& l : img.levels) {
        out.write(reinterpret_cast<const char*>(l.px.data()), std::streamsize(l.px.size()));
    }
    if (!out) LOGE("texture dump: %s not written", Utf8(path).c_str());
}

// PNG and other WIC files

IWICImagingFactory* Wic() {
    static ComPtr<IWICImagingFactory> wic;  // the worker's
    if (!wic) CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic));
    return wic.Get();
}

// level 0 as BGRA: WIC's DDS decoder unpacks the BC formats
HRESULT LevelZeroBitmap(IWICImagingFactory* wic, const Image& img, std::vector<uint8_t>& dds,
                        ComPtr<IWICBitmapSource>& out) {
    const Level& top = img.levels[0];
    if (Uncompressed(img.format)) {
        ComPtr<IWICBitmap> bitmap;
        HRESULT hr = wic->CreateBitmapFromMemory(img.width, img.height, GUID_WICPixelFormat32bppBGRA, top.pitch,
                                                 UINT(top.px.size()), const_cast<BYTE*>(top.px.data()), &bitmap);
        out = bitmap;
        return hr;
    }
    Image first{img.format, img.width, img.height, {top}};
    dds = DdsHeader(first);
    dds.insert(dds.end(), top.px.begin(), top.px.end());
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    HRESULT hr = wic->CreateStream(&stream);
    if (SUCCEEDED(hr)) hr = stream->InitializeFromMemory(dds.data(), DWORD(dds.size()));
    if (SUCCEEDED(hr)) {
        hr = wic->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &decoder);
    }
    if (SUCCEEDED(hr)) hr = decoder->GetFrame(0, &frame);
    if (SUCCEEDED(hr)) hr = WICConvertBitmapSource(GUID_WICPixelFormat32bppBGRA, frame.Get(), &out);
    return hr;
}

void WritePng(const fs::path& path, const Image& img) {
    std::error_code ec;
    if (fs::exists(path, ec)) return;
    IWICImagingFactory* wic = Wic();
    std::vector<uint8_t> dds;  // the source reads it until the frame is written
    ComPtr<IWICBitmapSource> bgra;
    ComPtr<IWICStream> file;
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    HRESULT hr = wic ? LevelZeroBitmap(wic, img, dds, bgra) : E_FAIL;
    if (SUCCEEDED(hr)) hr = wic->CreateStream(&file);
    if (SUCCEEDED(hr)) hr = file->InitializeFromFilename(path.c_str(), GENERIC_WRITE);
    if (SUCCEEDED(hr)) hr = wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder);
    if (SUCCEEDED(hr)) hr = encoder->Initialize(file.Get(), WICBitmapEncoderNoCache);
    if (SUCCEEDED(hr)) hr = encoder->CreateNewFrame(&frame, nullptr);
    if (SUCCEEDED(hr)) hr = frame->Initialize(nullptr);
    if (SUCCEEDED(hr)) hr = frame->SetSize(img.width, img.height);
    if (SUCCEEDED(hr)) hr = frame->SetPixelFormat(&format);
    if (SUCCEEDED(hr)) hr = frame->WriteSource(bgra.Get(), nullptr);
    if (SUCCEEDED(hr)) hr = frame->Commit();
    if (SUCCEEDED(hr)) hr = encoder->Commit();
    if (FAILED(hr)) LOGE("texture dump: %s not written (0x%08lx)", Utf8(path).c_str(), static_cast<unsigned long>(hr));
}

}  // namespace

void WriteBgraPng(const fs::path& path, uint32_t width, uint32_t height, const std::vector<uint8_t>& bgra) {
    Image img;
    img.format = DXGI_FORMAT_B8G8R8A8_UNORM;
    img.width = width;
    img.height = height;
    img.levels.push_back({bgra, width * 4});
    WritePng(path, img);
}

namespace {

bool ReadPng(const fs::path& path, Image& img) {
    IWICImagingFactory* wic = Wic();
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICBitmapSource> bgra;
    HRESULT hr = wic ? S_OK : E_FAIL;
    if (SUCCEEDED(hr)) {
        hr = wic->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand,
                                            &decoder);
    }
    if (SUCCEEDED(hr)) hr = decoder->GetFrame(0, &frame);
    if (SUCCEEDED(hr)) hr = WICConvertBitmapSource(GUID_WICPixelFormat32bppBGRA, frame.Get(), &bgra);
    UINT w = 0;
    UINT h = 0;
    if (SUCCEEDED(hr)) hr = bgra->GetSize(&w, &h);
    if (FAILED(hr) || !w || !h || w > kMaxSide || h > kMaxSide) return false;
    Level top{std::vector<uint8_t>(size_t(w) * h * 4), w * 4};
    if (FAILED(bgra->CopyPixels(nullptr, top.pitch, UINT(top.px.size()), top.px.data()))) return false;
    img.format = DXGI_FORMAT_B8G8R8A8_UNORM;
    img.width = w;
    img.height = h;
    img.levels.push_back(std::move(top));
    return true;
}

// Mipmaps

// one 2x2 box of src, its far row and column clamped to the level's edge
void AverageBox(const Level& src, uint32_t w, uint32_t h, uint32_t x, uint32_t y, uint8_t* out) {
    const uint8_t* row0 = &src.px[size_t(y * 2) * src.pitch];
    const uint8_t* row1 = &src.px[size_t(std::min(y * 2 + 1, h - 1)) * src.pitch];
    uint32_t x0 = x * 2 * 4;
    uint32_t x1 = std::min(x * 2 + 1, w - 1) * 4;
    for (uint32_t c = 0; c < 4; c++) {
        uint32_t sum = row0[x0 + c] + row0[x1 + c] + row1[x0 + c] + row1[x1 + c];
        out[c] = uint8_t((sum + 2) / 4);
    }
}

// a level with only level 0 gets a box filtered chain, so a bigger image does not shimmer at a distance
void AddMipmaps(Image& img) {
    if (img.levels.size() > 1 || !Uncompressed(img.format)) return;
    uint32_t w = img.width;
    uint32_t h = img.height;
    while (w > 1 || h > 1) {
        uint32_t half_w = std::max(w / 2, 1u);
        uint32_t half_h = std::max(h / 2, 1u);
        Level next{std::vector<uint8_t>(size_t(half_w) * half_h * 4), half_w * 4};
        for (uint32_t y = 0; y < half_h; y++) {
            uint8_t* row = &next.px[size_t(y) * next.pitch];
            for (uint32_t x = 0; x < half_w; x++) AverageBox(img.levels.back(), w, h, x, y, row + x * 4);
        }
        img.levels.push_back(std::move(next));
        w = half_w;
        h = half_h;
    }
}

// Loading

void Load(uint64_t key, const fs::path& path, uint32_t revision) {
    Image img;
    bool dds = _wcsicmp(path.extension().c_str(), L".dds") == 0;
    bool ok = dds ? ReadDds(path, img) : ReadPng(path, img);
    // a BC texture's top level is whole blocks
    if (ok && !Uncompressed(img.format) && ((img.width | img.height) & 3)) ok = false;
    if (ok) {
        AddMipmaps(img);
    } else {
        LOGE("texture pack: %s did not load", Utf8(path).c_str());
    }
    std::lock_guard l(g.lock);
    if (revision != g.revision) return;
    if (ok) {
        g.loaded[key] = std::move(img);
    } else {
        g.loaded[key] = std::nullopt;
    }
}

Image Decode(const TextureDesc& d, const uint8_t* guest, const uint32_t* palette) {
    Image img;
    img.format = UploadFormat(Format(d.format).host);
    img.width = d.width;
    img.height = d.height;
    img.levels.resize(d.levels);
    for (uint32_t l = 0; l < d.levels; l++) {
        Level& level = img.levels[l];
        DecodeLevel(d, l, 0, guest, palette, level.px, level.pitch);
    }
    return img;
}

// the settings into g.set; false when none changed
bool LatchSettings() {
    const app::Settings& s = app::Get();
    Latched now{s.custom_textures, s.dump_textures, s.texture_dump_format, s.ui_upscale};
    if (now == g.set) return false;
    g.set = now;
    fs::path root = app::DataDir() / "textures";
    g.pack_dir = root / "replacement";
    if (now.dumping) {
        g.dump_dir = root / "dumps";
        std::error_code ec;
        fs::create_directories(g.dump_dir, ec);
        fs::create_directories(g.pack_dir, ec);  // ready for the edited images
        LOGR("texture dump: %s into %s", now.format == kDumpDds ? "DDS" : "PNG", Utf8(g.dump_dir).c_str());
    }
    // a texture dumped in the other format is dumped again
    std::lock_guard l(g.lock);
    g.dumped.clear();
    return true;
}

// the pack folder's change handle, tried again now and then while the folder is missing; once made the pack is
// listed at once
void EnsureWatch() {
    if (g.watch != INVALID_HANDLE_VALUE || g.watch_tries++ % kWatchRetry) return;
    g.watch = FindFirstChangeNotificationW(g.pack_dir.c_str(), TRUE, kPackChanges);
    if (g.watch != INVALID_HANDLE_VALUE) g.changed = Clock::time_point{};
}

void NoteFolderChange(Clock::time_point now) {
    if (g.watch == INVALID_HANDLE_VALUE || WaitForSingleObject(g.watch, 0) != WAIT_OBJECT_0) return;
    g.changed = now;
    if (FindNextChangeNotification(g.watch)) return;
    FindCloseChangeNotification(g.watch);  // the folder went away
    g.watch = INVALID_HANDLE_VALUE;
}

// UI upscaling

// Scale2x (EPX) on labels: corners stay square, diagonal steps halve
std::vector<int32_t> Scale2x(const std::vector<int32_t>& m, uint32_t w, uint32_t h) {
    std::vector<int32_t> out(size_t(w) * h * 4);
    auto at = [&](int x, int y) {
        return m[size_t(std::clamp(y, 0, int(h) - 1)) * w + size_t(std::clamp(x, 0, int(w) - 1))];
    };
    size_t W = size_t(w) * 2;
    for (int y = 0; y < int(h); y++)
        for (int x = 0; x < int(w); x++) {
            int32_t p = at(x, y), a = at(x, y - 1), b = at(x + 1, y), c = at(x - 1, y), d = at(x, y + 1);
            int32_t* o = &out[size_t(y) * 2 * W + size_t(x) * 2];
            o[0] = c == a && c != d && a != b ? a : p;
            o[1] = a == b && a != c && b != d ? b : p;
            o[W] = d == c && d != b && c != a ? c : p;
            o[W + 1] = b == d && b != a && d != c ? d : p;
        }
    return out;
}

// Flat art: up to kMaxColours flat colours (transparency counted, colour of clear texels not) cover most texels -
// a font, frame and box pieces with their see-through fills, the green terminal icons and lines - with few gentle
// steps between neighbours. Each texel takes
// its flat colour's label, the labels are scaled by Scale2x three times (8x) and averaged 2x2 to kUiScale x, and each
// flat colour stays flat; a texel near none keeps its own colour, bilinear. Glows and gradients, painted pictures and
// noise (labels changing at most texels) are not flat: false, left as they are.
constexpr uint32_t kUiScale = 4;
bool UpscaleFlat(std::vector<uint8_t>& bgra, uint32_t& w, uint32_t& h) {
    constexpr int kMaxColours = 8;
    constexpr float kNear = 40;            // premultiplied BGRA distance, 0-255 units, within a flat colour
    constexpr double kMinCovered = 0.85;   // of the texels
    constexpr double kMaxChanges = 0.3;    // label changes between neighbours in a row: past this, noise
    size_t n = size_t(w) * h;
    if (n < 4) return false;
    std::vector<std::array<float, 4>> pm(n);
    for (size_t i = 0; i < n; i++) {
        float a = bgra[i * 4 + 3] / 255.0f;
        pm[i] = {bgra[i * 4] * a, bgra[i * 4 + 1] * a, bgra[i * 4 + 2] * a, float(bgra[i * 4 + 3])};
    }
    auto dist2 = [](const std::array<float, 4>& p, const std::array<float, 4>& q) {
        float s = 0;
        for (int c = 0; c < 4; c++) s += (p[c] - q[c]) * (p[c] - q[c]);
        return s;
    };
    // gradients and shading: many neighbours a small step apart (flat art steps nothing inside a shape, or a lot
    // at its edge)
    constexpr float kStep = 6;
    constexpr double kMaxGentle = 0.06;  // of neighbour pairs: clouds 0.26, a shaded coin 0.08, a font 0.01
    size_t gentle = 0, pairs = 0;
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++) {
            size_t i = size_t(y) * w + x;
            for (size_t j : {x + 1 < w ? i + 1 : i, y + 1 < h ? i + w : i}) {
                if (j == i) continue;
                float d = std::sqrt(dist2(pm[i], pm[j]));
                gentle += d > kStep && d < kNear;
                pairs++;
            }
        }
    if (pairs && gentle > pairs * kMaxGentle) return false;
    // the most common colours (to 8 levels a channel), each far enough from those taken before
    std::unordered_map<uint32_t, uint32_t> hist;
    auto bin = [](const std::array<float, 4>& p) {
        uint32_t k = 0;
        for (int c = 0; c < 4; c++) k = k << 8 | (uint32_t(p[c]) >> 3);
        return k;
    };
    for (const auto& p : pm) hist[bin(p)]++;
    std::vector<std::pair<uint32_t, uint32_t>> bins(hist.begin(), hist.end());
    std::sort(bins.begin(), bins.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    std::vector<std::array<float, 4>> centres;
    for (const auto& [k, count] : bins) {
        if (int(centres.size()) == kMaxColours) break;
        std::array<float, 4> c;
        for (int i = 0; i < 4; i++) c[3 - i] = float(((k >> (8 * i)) & 0xFF) * 8 + 4);
        bool close = false;
        for (const auto& t : centres) close |= dist2(c, t) <= kNear * kNear;
        if (!close) centres.push_back(c);
    }
    // a colour on the way between two flat colours is their blend (an anti-aliased edge, a glyph fading into its
    // box): it goes to the nearer end, so the edge comes out sharp instead of as a band of its own
    auto along = [](const std::array<float, 4>& c, const std::array<float, 4>& p, const std::array<float, 4>& q,
                    float& t) {
        float pq = 0, dot = 0;
        for (int k = 0; k < 4; k++) {
            pq += (q[k] - p[k]) * (q[k] - p[k]);
            dot += (c[k] - p[k]) * (q[k] - p[k]);
        }
        t = pq > 0 ? dot / pq : 0;
        if (t <= 0 || t >= 1) return 1e30f;
        float d = 0;
        for (int k = 0; k < 4; k++) {
            float e = c[k] - (p[k] + (q[k] - p[k]) * t);
            d += e * e;
        }
        return d;
    };
    std::vector<std::array<float, 4>> flat;
    for (const auto& c : centres) {
        bool blend = false;
        float t;
        for (size_t a = 0; a < flat.size() && !blend; a++)
            for (size_t b = a + 1; b < flat.size() && !blend; b++) blend = along(c, flat[a], flat[b], t) <= kNear * kNear;
        if (!blend) flat.push_back(c);
    }
    centres.swap(flat);
    if (centres.size() < 2) return false;
    std::vector<int32_t> label(n, -1);
    size_t covered = 0;
    for (size_t i = 0; i < n; i++) {
        float best = kNear * kNear;
        for (size_t c = 0; c < centres.size(); c++) {
            float d = dist2(pm[i], centres[c]);
            if (d <= best) {
                best = d;
                label[i] = int32_t(c);
            }
        }
        for (size_t a = 0; a < centres.size() && label[i] < 0; a++)
            for (size_t b = a + 1; b < centres.size(); b++) {
                float t, d = along(pm[i], centres[a], centres[b], t);
                if (d <= best) {
                    best = d;
                    label[i] = int32_t(t < 0.5f ? a : b);
                }
            }
        covered += label[i] >= 0;
    }
    size_t changes = 0;
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x + 1 < w; x++) changes += label[size_t(y) * w + x] != label[size_t(y) * w + x + 1];
    if (covered < n * kMinCovered || (w > 1 && changes > (n - h) * kMaxChanges)) return false;
    // each flat colour: its texels' average
    std::vector<std::array<double, 4>> sum(centres.size(), {0, 0, 0, 0});
    std::vector<size_t> members(centres.size());
    for (size_t i = 0; i < n; i++) {
        if (label[i] < 0) continue;
        for (int c = 0; c < 4; c++) sum[size_t(label[i])][c] += pm[i][c];
        members[size_t(label[i])]++;
    }
    for (size_t c = 0; c < centres.size(); c++)
        for (int k = 0; k < 4; k++) centres[c][k] = members[c] ? float(sum[c][k] / members[c]) : centres[c][k];
    std::vector<int32_t> big = label;
    uint32_t mw = w, mh = h;
    for (int pass = 0; pass < 3; pass++) {
        big = Scale2x(big, mw, mh);
        mw *= 2;
        mh *= 2;
    }
    uint32_t W = w * kUiScale, H = h * kUiScale;
    std::vector<uint8_t> out(size_t(W) * H * 4);
    // a texel near no flat colour: bilinear, premultiplied
    auto own = [&](float fx, float fy) {
        int x0 = int(std::floor(fx)), y0 = int(std::floor(fy));
        float tx = fx - x0, ty = fy - y0;
        auto at = [&](int x, int y) -> const std::array<float, 4>& {
            return pm[size_t(std::clamp(y, 0, int(h) - 1)) * w + size_t(std::clamp(x, 0, int(w) - 1))];
        };
        std::array<float, 4> v;
        for (int c = 0; c < 4; c++)
            v[c] = (at(x0, y0)[c] * (1 - tx) + at(x0 + 1, y0)[c] * tx) * (1 - ty) +
                   (at(x0, y0 + 1)[c] * (1 - tx) + at(x0 + 1, y0 + 1)[c] * tx) * ty;
        return v;
    };
    for (uint32_t y = 0; y < H; y++)
        for (uint32_t x = 0; x < W; x++) {
            float acc[4] = {};
            for (int sy = 0; sy < 2; sy++)
                for (int sx = 0; sx < 2; sx++) {
                    int32_t l = big[(size_t(y) * 2 + sy) * mw + size_t(x) * 2 + sx];
                    std::array<float, 4> v = l >= 0 ? centres[size_t(l)]
                                                    : own((x * 2 + sx + 0.5f) / (2 * kUiScale) - 0.5f,
                                                          (y * 2 + sy + 0.5f) / (2 * kUiScale) - 0.5f);
                    for (int c = 0; c < 4; c++) acc[c] += v[c] / 4;
                }
            uint8_t* o = &out[(size_t(y) * W + x) * 4];
            float a = acc[3];
            for (int c = 0; c < 3; c++) o[c] = uint8_t(std::clamp(a > 0 ? acc[c] * 255 / a : 0.0f, 0.0f, 255.0f) + 0.5f);
            o[3] = uint8_t(std::clamp(a, 0.0f, 255.0f) + 0.5f);
        }
    bgra.swap(out);
    w = W;
    h = H;
    return true;
}

// lists the pack once its folder is watched, then again once its files changed and settled; true when listed
bool UpdatePackFiles() {
    Clock::time_point now = Clock::now();
    EnsureWatch();
    NoteFolderChange(now);
    if (!g.changed || now - *g.changed < kSettle) return false;
    g.changed.reset();
    ListPack(g.pack_dir);
    std::lock_guard l(g.lock);
    g.revision++;
    g.asked.clear();
    g.loaded.clear();
    return true;
}

}  // namespace

bool Update() {
    bool changed = LatchSettings();
    bool reloaded = g.set.replacing && UpdatePackFiles();
    bool refresh = g_refresh.exchange(false);
    if (refresh) {
        g.edited.clear();
        std::lock_guard l(g.lock);
        g.revision++;
        g.asked.clear();
        g.loaded.clear();
    }
    if (changed || reloaded || refresh) g.generation++;
    return reloaded || refresh;
}

bool Edit(uint64_t key, const TextureDesc& d, const uint8_t* guest, const uint32_t* palette, bool ui) {
    const app::GameInfo& gi = app::kGame;
    bool game = key && gi.texture_edits && gi.texture_image && gi.texture_edits(key);
    constexpr uint32_t kMaxUiSide = 512;  // 2D art past this is a picture, not a font or a frame
    ui = ui && key && g.set.ui_upscale && d.width <= kMaxUiSide && d.height <= kMaxUiSide;
    if (!game && !ui) return false;
    auto [at, fresh] = g.edited.try_emplace(key, ui);
    if (!fresh) {
        if (!ui || at->second) return true;
        // seen in 3D before, now in 2D: made again
        at->second = true;
        std::lock_guard l(g.lock);
        g.loaded.erase(key);
    }
    uint32_t revision;
    {
        std::lock_guard l(g.lock);
        revision = g.revision;
    }
    TextureDesc top = d;
    top.levels = 1;
    Post([key, revision, ui, game, img = Decode(top, guest, palette)] {
        IWICImagingFactory* wic = Wic();
        std::vector<uint8_t> dds;
        ComPtr<IWICBitmapSource> src;
        std::vector<uint8_t> px(size_t(img.width) * img.height * 4);
        bool ok = wic && SUCCEEDED(LevelZeroBitmap(wic, img, dds, src)) &&
                  SUCCEEDED(src->CopyPixels(nullptr, img.width * 4, UINT(px.size()), px.data()));
        uint32_t w = img.width, h = img.height;
        uint32_t sw = w, sh = h;
        bool upscaled = ok && ui && UpscaleFlat(px, w, h);
        if (ok && ui) {
            LOGI("ui texture %016llx %ux%u: %s", (unsigned long long)key, sw, sh, upscaled ? "flat, drawn 4x" : "left");
        }
        bool edited = ok && game && app::kGame.texture_image(key, w, h, px);
        ok = (upscaled || edited) && w && h && px.size() == size_t(w) * h * 4;
        std::optional<Image> made;
        if (ok) {
            made.emplace();
            made->format = DXGI_FORMAT_B8G8R8A8_UNORM;
            made->width = w;
            made->height = h;
            made->levels.push_back({std::move(px), w * 4});
            AddMipmaps(*made);
        }
        std::lock_guard l(g.lock);
        if (revision == g.revision) g.loaded[key] = std::move(made);
    });
    return true;
}

uint32_t Generation() { return g.generation; }

bool Replacing() { return g.set.replacing && !g.files.empty(); }

uint64_t Key(const TextureDesc& d, const FormatInfo& fi, const uint8_t* guest, const uint32_t* palette) {
    if (!Replacing() && !g.set.dumping && !g.set.ui_upscale && !app::kGame.texture_edits) return 0;
    // cube maps, volumes and depth stay the console's; linear ones are movie frames and the game's own copies
    if (d.cube || d.depth > 1 || d.linear || UploadFormat(fi.host) == DXGI_FORMAT_UNKNOWN) return 0;
    uint32_t shape[] = {d.format, d.width, d.height};
    uint64_t key = Hash(guest, LevelSize(d, 0), Hash(shape, sizeof(shape)));
    if (palette) key = Hash(palette, kPaletteBytes, key);
    return key ? key : 1;
}

void Dump(uint64_t key, const TextureDesc& d, const uint8_t* guest, const uint32_t* palette) {
    if (!key || !g.set.dumping || g.files.count(key)) return;
    {
        std::lock_guard l(g.lock);
        if (!g.dumped.insert(key).second) return;
    }
    bool dds = g.set.format == kDumpDds;
    fs::path path = g.dump_dir / DumpName(key, d, dds ? "dds" : "png");
    Post([path, dds, img = Decode(d, guest, palette)] {
        if (dds) {
            WriteDds(path, img);
        } else {
            WritePng(path, img);
        }
    });
}

Lookup Take(uint64_t key, Image& out) {
    if (g.edited.count(key)) {
        std::lock_guard l(g.lock);
        auto it = g.loaded.find(key);
        if (it == g.loaded.end()) return Lookup::Loading;
        if (!it->second) return Lookup::None;
        out = std::move(*it->second);
        g.loaded.erase(it);
        return Lookup::Ready;
    }
    auto file = g.files.find(key);
    if (!g.set.replacing || file == g.files.end()) return Lookup::None;
    uint32_t revision = 0;
    {
        std::lock_guard l(g.lock);
        auto it = g.loaded.find(key);
        if (it != g.loaded.end()) {
            if (!it->second) return Lookup::None;
            out = std::move(*it->second);
            g.loaded.erase(it);
            return Lookup::Ready;
        }
        if (!g.asked.insert(key).second) return Lookup::Loading;
        revision = g.revision;
    }
    Post([key, path = file->second, revision] { Load(key, path, revision); });
    return Lookup::Loading;
}

}  // namespace gpu::texturedumper

void app::RefreshTextureImages() { gpu::texturedumper::g_refresh = true; }
