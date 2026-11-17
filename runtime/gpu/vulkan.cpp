// The title's Direct3D 8 calls drawn with Vulkan: the D3D12 renderer's (d3d12.cpp) logic on Vulkan 1.3, for Linux and
// Windows. Its targets, texture cache, HUD placement, constants, captures and present pass follow d3d12.cpp's step by
// step; what differs is the API: dynamic rendering, push descriptors laid out as the D3D12 root signature (b0 b1, t0..t3,
// s0..s3), a negative viewport height so the shaders keep D3D's clip space, the HLSL compiled to SPIR-V by glslang.
// volk loads Vulkan at run time, VMA allocates. Not ported yet: renderer extensions (gpu/ext.h) and HDR10 output.
#ifdef XBE_VULKAN
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#include <volk.h>
#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1
#define VMA_IMPLEMENTATION
#include <vk_mem_alloc.h>

#ifndef _WIN32
#include <SDL3/SDL_vulkan.h>
#include <SDL3/SDL_video.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <deque>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#include <imgui.h>
#ifdef _WIN32
#include <backends/imgui_impl_win32.h>
#else
#include <backends/imgui_impl_sdl3.h>
#endif
#include <backends/imgui_impl_vulkan.h>

#include "app/app.h"
#include "core/diag.h"
#include "core/log.h"
#include "core/platform.h"
#include "gpu/backend.h"
#include "gpu/dxgi_format.h"
#include "gpu/shaders.h"
#include "gpu/spirv.h"
#include "gpu/texture.h"
#include "gpu/texturedumper.h"
#include "gpu/vertex.h"
#include "kernel/kernel.h"

// FSR 1's constants on the CPU side (third_party/fsr1, MIT); its shaders, as SPIR-V, in post/present_spirv.h
#define A_CPU 1
#include "../../third_party/fsr1/ffx_a.h"
#include "../../third_party/fsr1/ffx_fsr1.h"
#include "gpu/post/present_spirv.h"

namespace gpu {

namespace {

constexpr int kFrames = 3;
constexpr uint64_t kStaleFrames = 8;
constexpr uint64_t kUploadSize = 64ull << 20;
constexpr uint64_t kUploadReserve = 8ull << 20;
constexpr VkFormat kBgra = VK_FORMAT_B8G8R8A8_UNORM;
constexpr VkFormat kDepthFormat = VK_FORMAT_D32_SFLOAT_S8_UINT;
constexpr float kDepthFar = 0;
constexpr uint32_t kQueries = 8192;
constexpr uint32_t kNoQuery = ~0u;

VkDevice g_dev = VK_NULL_HANDLE;
VmaAllocator g_vma = nullptr;

// The window the swap chain is made for: SDL's on Linux, the Win32 host's on Windows
std::vector<const char*> SurfaceExtensions() {
#ifdef _WIN32
    return {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME};
#else
    Uint32 n = 0;
    const char* const* ext = SDL_Vulkan_GetInstanceExtensions(&n);
    if (!ext) LOGE("vulkan: %s", SDL_GetError());
    return ext ? std::vector<const char*>(ext, ext + n) : std::vector<const char*>{};
#endif
}

bool CreateSurface(VkInstance instance, VkSurfaceKHR* surface) {
#ifdef _WIN32
    VkWin32SurfaceCreateInfoKHR ci{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
    ci.hinstance = GetModuleHandleW(nullptr);
    ci.hwnd = static_cast<HWND>(app::Window());
    return vkCreateWin32SurfaceKHR(instance, &ci, nullptr, surface) == VK_SUCCESS;
#else
    if (SDL_Vulkan_CreateSurface(static_cast<SDL_Window*>(app::Window()), instance, nullptr, surface)) return true;
    LOGE("vulkan: %s", SDL_GetError());
    return false;
#endif
}

// the window's client area in pixels
void WindowPixels(int& w, int& h) {
#ifdef _WIN32
    RECT rc{};
    GetClientRect(static_cast<HWND>(app::Window()), &rc);
    w = rc.right - rc.left;
    h = rc.bottom - rc.top;
#else
    SDL_GetWindowSizeInPixels(static_cast<SDL_Window*>(app::Window()), &w, &h);
#endif
}

void UiNewFrame() {
#ifdef _WIN32
    ImGui_ImplWin32_NewFrame();
#else
    UiNewFrame();
#endif
}

// perf diagnostic, as d3d12.cpp's
using PerfClock = std::chrono::steady_clock;
double PerfMs(PerfClock::time_point t) { return std::chrono::duration<double, std::milli>(PerfClock::now() - t).count(); }
struct Perf {
    double gpu = 0;  // the GPU's time from a frame's first command to its present's (timestamps)
    double draw = 0, assemble = 0, bind = 0, sig = 0, texup = 0, compile = 0, clear = 0, copy = 0, present = 0,
           wait = 0, swap = 0, sync = 0;
    uint64_t draws = 0, verts = 0, upload = 0, overflow = 0, sigs = 0, sig_bytes = 0, texups = 0, texup_bytes = 0,
             newtex = 0, targets = 0, rtups = 0, shrinks = 0, syncs = 0, psos = 0, shaders = 0, raw = 0, lates = 0;
};
Perf g_perf;
struct PerfScope {
    double& acc;
    bool on = diag::Perf();
    PerfClock::time_point t = on ? PerfClock::now() : PerfClock::time_point{};
    ~PerfScope() {
        if (on) acc += PerfMs(t);
    }
};

FILE* OpenFile(const std::filesystem::path& p, const char* mode) {
#ifdef _WIN32
    std::wstring m(mode, mode + std::strlen(mode));
    return _wfopen(p.c_str(), m.c_str());
#else
    return std::fopen(p.c_str(), mode);
#endif
}

// cache/shaders-vk.bin: SPIR-V by source hash, as d3d12.cpp's shaders.bin holds its bytecode
constexpr uint32_t kShaderCacheMagic = 0x4B565355;  // "USVK"
constexpr uint32_t kShaderCacheVersion = 1;
class ShaderDiskCache {
public:
    const std::vector<uint32_t>* Find(uint64_t hash) {
        std::lock_guard lock(mutex_);
        Open();
        auto it = blobs_.find(hash);
        return it == blobs_.end() ? nullptr : &it->second;
    }
    void Add(uint64_t hash, const std::vector<uint32_t>& code) {
        std::lock_guard lock(mutex_);
        Open();
        if (!file_ || blobs_.count(hash)) return;
        uint32_t n = uint32_t(code.size() * 4);
        fwrite(&hash, 8, 1, file_);
        fwrite(&n, 4, 1, file_);
        fwrite(code.data(), 1, n, file_);
        fflush(file_);
        blobs_[hash] = code;
    }

private:
    void Open() {
        if (opened_) return;
        opened_ = true;
        std::error_code ec;
        std::filesystem::path dir = app::DataDir() / "cache";
        std::filesystem::create_directories(dir, ec);
        std::filesystem::path path =
            dir / (app::Instance() > 1 ? "shaders-vk" + std::to_string(app::Instance()) + ".bin" : std::string("shaders-vk.bin"));
        if (FILE* f = OpenFile(path, "rb")) {
            uint32_t head[2] = {};
            if (fread(head, 4, 2, f) == 2 && head[0] == kShaderCacheMagic && head[1] == kShaderCacheVersion) {
                uint64_t hash;
                uint32_t n;
                while (fread(&hash, 8, 1, f) == 1 && fread(&n, 4, 1, f) == 1 && n <= (4u << 20) && !(n & 3)) {
                    std::vector<uint32_t> b(n / 4);
                    if (fread(b.data(), 1, n, f) != n) break;
                    blobs_[hash] = std::move(b);
                }
            }
            fclose(f);
        }
        // rewritten whole, so appends land after the last good record
        file_ = OpenFile(path, "wb");
        if (file_) {
            uint32_t head[2] = {kShaderCacheMagic, kShaderCacheVersion};
            fwrite(head, 4, 2, file_);
            for (auto& [h, b] : blobs_) {
                uint32_t n = uint32_t(b.size() * 4);
                fwrite(&h, 8, 1, file_);
                fwrite(&n, 4, 1, file_);
                fwrite(b.data(), 1, n, file_);
            }
            fflush(file_);
        }
        LOGI("vulkan: shader cache %s, %zu shaders", path.string().c_str(), blobs_.size());
    }
    std::mutex mutex_;
    bool opened_ = false;
    FILE* file_ = nullptr;
    std::unordered_map<uint64_t, std::vector<uint32_t>> blobs_;
};
ShaderDiskCache g_shader_disk;

constexpr uint32_t kPipelineCacheMagic = 0x4B564355;  // "UCVK"
constexpr uint32_t kPipelineCacheVersion = 1;

bool CaptureOn() {
    if (diag::CaptureDir().empty()) LOG_FIRST(1, "capture: set XBE_CAPTURE_DIR");
    return !diag::CaptureDir().empty();
}

const char* kBlitHlsl = R"(
cbuffer B : register(b1) { float4 uvrect; float4 opts; };
Texture2D img : register(t0);
Texture2D lut : register(t1);
SamplerState ls : register(s0);
SamplerState ps : register(s1);
struct O { float4 p : SV_Position; float2 uv : TEXCOORD0; };
O vs(uint id : SV_VertexID) {
    O o;
    float2 t = float2((id << 1) & 2, id & 2);
    o.p = float4(t * float2(2, -2) + float2(-1, 1), 0, 1);
    o.uv = uvrect.xy + t * uvrect.zw;
    return o;
}
float4 psmain(O i) : SV_Target {
    float4 c = 0;
    int n = max((int)opts.z, 1);
    float2 px = fwidth(i.uv);
    for (int y = 0; y < n; y++)
        for (int x = 0; x < n; x++) c += img.SampleLevel(ls, i.uv + px * ((float2(x, y) + 0.5) / n - 0.5), 0);
    c /= n * n;
    c.rgb *= opts.w;
    if (opts.x > 0) c.rgb = float3(lut.Sample(ps, float2(c.r, 0.5)).r, lut.Sample(ps, float2(c.g, 0.5)).g, lut.Sample(ps, float2(c.b, 0.5)).b);
    return float4(c.rgb, opts.y > 0 ? c.a : 1);
}
)";

const char* kDepthHlsl = R"(
Texture2D<float> img : register(t0);
SamplerState ps : register(s1);
struct O { float4 p : SV_Position; float2 uv : TEXCOORD0; };
O vs(uint id : SV_VertexID) {
    O o;
    o.uv = float2((id << 1) & 2, id & 2);
    o.p = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}
float psmain(O i) : SV_Depth { return img.SampleLevel(ps, i.uv, 0); }
)";

const char* kClearHlsl = R"(
cbuffer B : register(b1) { float4 colour; };
float4 vs(uint id : SV_VertexID) : SV_Position {
    float2 t = float2((id << 1) & 2, id & 2);
    return float4(t * float2(2, -2) + float2(-1, 1), 0, 1);
}
float4 psmain() : SV_Target { return colour; }
)";

float Bits(uint32_t v) {
    float f;
    std::memcpy(&f, &v, 4);
    return f;
}

void Color(uint32_t argb, float* o) {
    o[0] = ((argb >> 16) & 255) / 255.0f;
    o[1] = ((argb >> 8) & 255) / 255.0f;
    o[2] = (argb & 255) / 255.0f;
    o[3] = (argb >> 24) / 255.0f;
}

constexpr uint32_t kBlendDestColor = 0x306;

VkBlendFactor Blend(uint32_t v, bool alpha) {
    switch (v) {
        case 0: return VK_BLEND_FACTOR_ZERO;
        case 1: return VK_BLEND_FACTOR_ONE;
        case 0x300: return alpha ? VK_BLEND_FACTOR_SRC_ALPHA : VK_BLEND_FACTOR_SRC_COLOR;
        case 0x301: return alpha ? VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA : VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
        case 0x302: return VK_BLEND_FACTOR_SRC_ALPHA;
        case 0x303: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        case 0x304: return VK_BLEND_FACTOR_DST_ALPHA;
        case 0x305: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
        case kBlendDestColor: return alpha ? VK_BLEND_FACTOR_DST_ALPHA : VK_BLEND_FACTOR_DST_COLOR;
        case 0x307: return alpha ? VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA : VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
        case 0x308: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
        case 0x8001: case 0x8003: return VK_BLEND_FACTOR_CONSTANT_COLOR;
        case 0x8002: case 0x8004: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
        default: return VK_BLEND_FACTOR_ONE;
    }
}

VkBlendOp BlendOp(uint32_t v) {
    switch (v) {
        case 0x800A: return VK_BLEND_OP_SUBTRACT;
        case 0x800B: return VK_BLEND_OP_REVERSE_SUBTRACT;
        case 0x8007: return VK_BLEND_OP_MIN;
        case 0x8008: return VK_BLEND_OP_MAX;
        default: return VK_BLEND_OP_ADD;
    }
}

// D3DCMP_NEVER 0x200 to D3DCMP_ALWAYS 0x207: Vulkan's order from 0, D3D12's from 1 (the shaders take D3D12's minus 1)
VkCompareOp Cmp(uint32_t v) { return VkCompareOp(v >= 0x200 && v <= 0x207 ? v - 0x200 : 7); }

VkStencilOp StencilOp(uint32_t v) {
    switch (v) {
        case 0: return VK_STENCIL_OP_ZERO;
        case 0x1E01: return VK_STENCIL_OP_REPLACE;
        case 0x1E02: return VK_STENCIL_OP_INCREMENT_AND_CLAMP;
        case 0x1E03: return VK_STENCIL_OP_DECREMENT_AND_CLAMP;
        case 0x150A: return VK_STENCIL_OP_INVERT;
        case 0x8507: return VK_STENCIL_OP_INCREMENT_AND_WRAP;
        case 0x8508: return VK_STENCIL_OP_DECREMENT_AND_WRAP;
        default: return VK_STENCIL_OP_KEEP;
    }
}

VkSamplerAddressMode Address(uint32_t v) {
    switch (v) {
        case 2: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
        case 3: case 5: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        case 4: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        default: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    }
}

VkFormat HostFormat(Host h) {
    switch (h) {
        case Host::BC1: return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
        case Host::BC2: return VK_FORMAT_BC2_UNORM_BLOCK;
        case Host::BC3: return VK_FORMAT_BC3_UNORM_BLOCK;
        case Host::R32F: return VK_FORMAT_R32_SFLOAT;
        default: return kBgra;
    }
}

// the texture pack's images arrive as DXGI formats
VkFormat FromDxgi(DXGI_FORMAT f) {
    switch (f) {
        case DXGI_FORMAT_BC1_UNORM: return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
        case DXGI_FORMAT_BC2_UNORM: return VK_FORMAT_BC2_UNORM_BLOCK;
        case DXGI_FORMAT_BC3_UNORM: return VK_FORMAT_BC3_UNORM_BLOCK;
        case DXGI_FORMAT_BC7_UNORM: return VK_FORMAT_BC7_UNORM_BLOCK;
        case DXGI_FORMAT_R8G8B8A8_UNORM: return VK_FORMAT_R8G8B8A8_UNORM;
        default: return kBgra;
    }
}

// vertex formats come as DXGI formats from vertex.cpp
VkFormat VertexFormat(uint32_t dxgi) {
    switch (dxgi) {
        case DXGI_FORMAT_R32_FLOAT: return VK_FORMAT_R32_SFLOAT;
        case DXGI_FORMAT_R32G32_FLOAT: return VK_FORMAT_R32G32_SFLOAT;
        case DXGI_FORMAT_R32G32B32_FLOAT: return VK_FORMAT_R32G32B32_SFLOAT;
        case DXGI_FORMAT_R32G32B32A32_FLOAT: return VK_FORMAT_R32G32B32A32_SFLOAT;
        case DXGI_FORMAT_B8G8R8A8_UNORM: return VK_FORMAT_B8G8R8A8_UNORM;
        case DXGI_FORMAT_R16_SNORM: return VK_FORMAT_R16_SNORM;
        case DXGI_FORMAT_R16G16_SNORM: return VK_FORMAT_R16G16_SNORM;
        case DXGI_FORMAT_R16G16B16A16_SNORM: return VK_FORMAT_R16G16B16A16_SNORM;
        case DXGI_FORMAT_R8_UNORM: return VK_FORMAT_R8_UNORM;
        case DXGI_FORMAT_R8G8_UNORM: return VK_FORMAT_R8G8_UNORM;
        case DXGI_FORMAT_R8G8B8A8_UNORM: return VK_FORMAT_R8G8B8A8_UNORM;
        case DXGI_FORMAT_R16_SINT: return VK_FORMAT_R16_SINT;
        case DXGI_FORMAT_R16G16_SINT: return VK_FORMAT_R16G16_SINT;
        case DXGI_FORMAT_R16G16B16A16_SINT: return VK_FORMAT_R16G16B16A16_SINT;
        case DXGI_FORMAT_R32_UINT: return VK_FORMAT_R32_UINT;
        default: return VK_FORMAT_R32G32B32A32_SFLOAT;
    }
}

bool IsBc(VkFormat f) {
    return f == VK_FORMAT_BC1_RGBA_UNORM_BLOCK || f == VK_FORMAT_BC2_UNORM_BLOCK || f == VK_FORMAT_BC3_UNORM_BLOCK ||
           f == VK_FORMAT_BC7_UNORM_BLOCK;
}
uint32_t BlockBytes(VkFormat f) { return f == VK_FORMAT_BC1_RGBA_UNORM_BLOCK ? 8 : 16; }
uint32_t TexelBytes(VkFormat f) {
    switch (f) {
        case VK_FORMAT_R32G32B32A32_SFLOAT: return 16;
        default: return 4;
    }
}

constexpr uint32_t kFmtD16 = 0x2C;
constexpr uint32_t kFmtLinD16 = 0x30;
uint8_t ShadowDepthKind(uint32_t format) { return format == kFmtD16 || format == kFmtLinD16 ? 2 : 1; }
uint32_t GuestPitch(uint32_t width) { return (width * 4 + 63) & ~63u; }

// An image, its memory and views, destroyed when the last reference goes: a frame's garbage holds the ones the GPU
// may still read until that frame's work is done
struct Image {
    VkImage img = VK_NULL_HANDLE;
    VmaAllocation mem = nullptr;
    VkImageView view = VK_NULL_HANDLE;  // sampled: 2D, 3D or cube, the depth aspect of a depth image
    VkImageView att = VK_NULL_HANDLE;   // attachment: 2D, every aspect
    VkFormat fmt = kBgra;
    uint32_t w = 1, h = 1, d = 1, levels = 1, layers = 1;
    bool depth = false;
    bool owned = true;  // false: a swap chain image, the swap chain's to destroy
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    ~Image() {
        if (view) vkDestroyImageView(g_dev, view, nullptr);
        if (att) vkDestroyImageView(g_dev, att, nullptr);
        if (img && owned) vmaDestroyImage(g_vma, img, mem);
    }
};
using ImageRef = std::shared_ptr<Image>;

struct Buffer {
    VkBuffer buf = VK_NULL_HANDLE;
    VmaAllocation mem = nullptr;
    uint8_t* map = nullptr;
    VkDeviceSize size = 0;
    ~Buffer() {
        if (buf) vmaDestroyBuffer(g_vma, buf, mem);
    }
};
using BufferRef = std::shared_ptr<Buffer>;

VkImageAspectFlags Aspects(const Image& i) {
    return i.depth ? VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
}

// a 2D image with one level unless levels says more; cube: six layers; d > 1: a 3D image
ImageRef MakeImage(VkFormat fmt, uint32_t w, uint32_t h, VkImageUsageFlags usage, bool depth = false,
                   uint32_t levels = 1, uint32_t d = 1, bool cube = false) {
    auto r = std::make_shared<Image>();
    r->fmt = fmt;
    r->w = w;
    r->h = h;
    r->d = d;
    r->levels = levels;
    r->layers = cube ? 6 : 1;
    r->depth = depth;
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.flags = cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0;
    ci.imageType = d > 1 ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
    ci.format = fmt;
    ci.extent = {w, h, d};
    ci.mipLevels = levels;
    ci.arrayLayers = r->layers;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = usage;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VmaAllocationCreateInfo ai{};
    ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    if (vmaCreateImage(g_vma, &ci, &ai, &r->img, &r->mem, nullptr) != VK_SUCCESS) return nullptr;
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = r->img;
    vi.viewType = cube ? VK_IMAGE_VIEW_TYPE_CUBE : d > 1 ? VK_IMAGE_VIEW_TYPE_3D : VK_IMAGE_VIEW_TYPE_2D;
    vi.format = fmt;
    vi.subresourceRange = {depth ? VkImageAspectFlags(VK_IMAGE_ASPECT_DEPTH_BIT) : VkImageAspectFlags(VK_IMAGE_ASPECT_COLOR_BIT),
                           0, levels, 0, r->layers};
    if (usage & VK_IMAGE_USAGE_SAMPLED_BIT) vkCreateImageView(g_dev, &vi, nullptr, &r->view);
    if (usage & (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)) {
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.subresourceRange.aspectMask = Aspects(*r);
        vi.subresourceRange.levelCount = 1;
        vi.subresourceRange.layerCount = 1;
        vkCreateImageView(g_dev, &vi, nullptr, &r->att);
    }
    return r;
}

BufferRef MakeBuffer(VkDeviceSize size, VkBufferUsageFlags usage, bool readback) {
    auto r = std::make_shared<Buffer>();
    r->size = size;
    VkBufferCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    ci.size = size;
    ci.usage = usage;
    VmaAllocationCreateInfo ai{};
    ai.usage = VMA_MEMORY_USAGE_AUTO;
    ai.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT |
               (readback ? VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT : VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT);
    // uploads are written and read back without flushes: coherent memory only
    ai.requiredFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | (readback ? 0 : VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VmaAllocationInfo info{};
    if (vmaCreateBuffer(g_vma, &ci, &ai, &r->buf, &r->mem, &info) != VK_SUCCESS) return nullptr;
    r->map = static_cast<uint8_t*>(info.pMappedData);
    return r;
}

// MemSig, as d3d12.cpp's
constexpr uint32_t kSigWholeMax = 65536;
constexpr uint32_t kSigSamples = 64;
constexpr uint32_t kSigSampleBytes = 64;

uint64_t MemSig(uint32_t data, uint32_t total) {
    const uint8_t* p = static_cast<const uint8_t*>(kernel::HostPtr(data));
    uint64_t sig = Hash(&total, sizeof(total));
    g_perf.sigs++;
    g_perf.sig_bytes += total <= kSigWholeMax ? total : kSigSamples * kSigSampleBytes;
    if (total <= kSigWholeMax) return Hash(p, total, sig);
    for (uint32_t i = 0; i < kSigSamples; i++) {
        const uint8_t* sample = p + uint64_t(total - kSigSampleBytes) * i / (kSigSamples - 1);
        sig = Hash(sample, kSigSampleBytes, sig);
    }
    return sig;
}

// a console surface the renderer draws into
struct VTarget {
    ImageRef img;
    ImageRef fb;  // copy read by draws that sample the target they render to
    bool depth = false;
    uint32_t gw = 0, gh = 0, w = 0, h = 0;
    uint32_t data = 0, bytes = 0;
    uint64_t epoch = 0, sig = 0;
    bool drawn = false;
    std::vector<uint8_t> read;
    VkFormat fmt = kBgra;
};

bool Changed(const VTarget& t) {
    if (t.drawn || t.read.empty()) return false;
    uint32_t row = t.gw * 4;
    uint32_t pitch = GuestPitch(t.gw);
    const uint8_t* p = static_cast<const uint8_t*>(kernel::HostPtr(t.data));
    for (uint32_t y = 0; y < t.gh; y++) {
        if (std::memcmp(p + size_t(y) * pitch, &t.read[size_t(y) * row], row)) return true;
    }
    return false;
}

bool LatchCpuWrite(VTarget& t) {
    uint64_t e = DirtyEpoch(t.data);
    if (e <= t.epoch) return false;
    t.epoch = e;
    uint64_t sig = MemSig(t.data, t.bytes);
    if (sig == t.sig && !Changed(t)) return false;
    t.sig = sig;
    return true;
}

struct Tex {
    ImageRef img;
    TextureDesc d;
    VkFormat fmt = kBgra;
    uint32_t levels = 1;
    uint64_t sig = 0;
    uint64_t frame = 0;
    uint64_t palette = 0;
    const Tex* swap = nullptr;
    bool ui = false;
    uint64_t pack_key = 0;
    uint32_t pack_gen = 0;
};

struct PsoKey {
    uint64_t vs;
    uint64_t ps;
    uint32_t regs;
    uint32_t topo;
    uint32_t has_rt;
    uint32_t has_ds;
    uint32_t elements[16];  // per register: DXGI format, buffer << 8, offset << 16
    uint32_t blend;
    uint32_t src;
    uint32_t dst;
    uint32_t op;
    uint32_t wmask;
    uint32_t zen;
    uint32_t zwrite;
    uint32_t zfunc;
    uint32_t sten;
    uint32_t sfunc;
    uint32_t smask;
    uint32_t swmask;
    uint32_t sfail;
    uint32_t szfail;
    uint32_t spass;
    uint32_t cull;
    uint32_t ccw;
    uint32_t fill;
    int32_t zbias;
    float zslope;
    uint32_t rtfmt;
    uint32_t sprite;
};

struct BlitOptions {
    bool gamma = false;
    bool alpha = false;
    uint32_t taps = 1;
    float mul = 1;
    VkFormat rtv = kBgra;
    bool over = false;
};

struct ShrinkOptions {
    uint32_t taps = 1;
    ImageRef into;
    float mul = 1;
};

// what a draw binds: the push descriptor set's contents
struct Descriptors {
    VkBuffer ubo[2] = {};
    VkDeviceSize off[2] = {};
    VkDeviceSize range[2] = {};
    VkImageView view[4] = {};
    VkSampler smp[4] = {};
    bool operator==(const Descriptors& o) const { return !std::memcmp(this, &o, sizeof(*this)); }
};

struct DrawTextures {
    TexInfo info[4];
    Image* res[4] = {};
    VkImageViewType type[4] = {};
    uint32_t levels[4] = {1, 1, 1, 1};
    bool rt[4] = {};
    float k[4] = {1, 1, 1, 1};
    float wide[4] = {};
    uint32_t tsize[4][2] = {};
};

// the attachments a draw renders to
struct Attach {
    Image* color = nullptr;
    Image* depth = nullptr;
    bool operator==(const Attach& o) const { return color == o.color && depth == o.depth; }
};

struct DrawBinds {
    VkPipeline pso = VK_NULL_HANDLE;
    bool rasterizer = false;
    VkViewport viewport{};
    VkRect2D scissor{};
    bool output = false;
    float blend[4] = {};
    uint32_t stencil = 0;
    bool descriptors = false;
    Descriptors set;
};

struct UsedTarget {
    uint32_t data = 0;
    uint64_t frame = UINT64_MAX;
};

struct DrawShaders {
    bool prog = false;
    bool quad = false;
    bool fixed_transform = false;
    bool sprite = false;
    FixedKey fk;
    uint64_t vkey = 0;
    uint64_t vh = 0;
    uint64_t pkey = 0;
    uint64_t ph = 0;
    VkShaderModule vs_code = VK_NULL_HANDLE;
    VkShaderModule ps_code = VK_NULL_HANDLE;
};

struct GuestExtent {
    uint32_t w;
    uint32_t h;
};

uint32_t SimpleState(const State& s, uint32_t method) { return s.simple[method >> 2]; }

class VulkanBackend final : public Backend {
public:
    ~VulkanBackend() override = default;
    bool Init(uint32_t width, uint32_t height) override;
    void Clear(const State& s, uint32_t count, const uint8_t* rects, uint32_t flags, uint32_t color, float z,
               uint32_t stencil) override;
    void Draw(const State& s, uint32_t prim, uint32_t start, uint32_t count, const uint8_t* indices) override;
    void Copy(const Surface& src, const Surface& dst, uint32_t count, uint32_t src_rects, uint32_t dst_points,
              float src_kx, float src_ky) override;
    void Present(const Surface& front) override;
    void WaitGpuFrame(uint64_t n) override;
    void Sync(uint32_t data, uint32_t size) override;
    void SyncLate(uint32_t data) override;
    bool DeliverLate(uint32_t data) override;
    void RecordLate(uint32_t data) override;
    void Flush() override;
    void SetGamma(const uint8_t* ramp) override;
    void ClearReport() override { counted_.clear(); }
    void GetReport(uint32_t report) override { reports_.push_back({report, counted_, timeline_value_ + 1}); }

private:
    struct Frame {
        VkCommandPool pool = VK_NULL_HANDLE;
        VkCommandBuffer cb = VK_NULL_HANDLE;
        BufferRef upload;
        uint64_t used = 0;
        uint64_t fence = 0;
        std::vector<ImageRef> garbage;
        std::vector<BufferRef> garbage_buffers;
        VkSemaphore acquired = VK_NULL_HANDLE;
    };
    struct Alloc {
        VkBuffer buf;
        VkDeviceSize off;
        uint8_t* cpu;
    };

    // Init, in order
    bool CreateInstance();
    bool CreateDevice();
    bool CreateSwapchain();
    void DestroySwapchain();
    bool CreateLayout();
    void CreateFrames();
    void CreateDummies();
    void CreateUi();
    void LoadSettings();

    Frame& CurrentFrame() { return frames_[frame_ % kFrames]; }
    void Begin();
    void Submit(bool wait, bool present = false);
    void WaitTimeline(uint64_t value);
    uint64_t Completed();
    Alloc Upload(uint64_t size, uint64_t align = 256);
    void Transition(Image& i, VkImageLayout want);
    void EndRendering();
    void EnsureRendering();
    void SetFullViewport(uint32_t w, uint32_t h);
    void SetBlitTarget(Image& i);
    void Blit(Image& src, const float uvrect[4], const BlitOptions& o);
    VkSampler EnsureSampler(const VkSamplerCreateInfo& ci);
    void BlitSamplers(VkSampler out[4]);
    void PushDescriptors(const Descriptors& d);
    void DrawFullScreen(VkPipeline pso, const Alloc& cb, Image* const views[4], const VkSampler* samplers = nullptr);
    Image* Feedback(VTarget& t);
    VkShaderModule Module(const std::vector<uint32_t>& code);
    VkShaderModule Compile(const std::string& src, const char* entry, const char* profile, uint64_t& hash);
    VkPipeline FullScreenPipeline(VkShaderModule vs, VkShaderModule ps, VkFormat color, bool depth_only,
                                  const VkPipelineColorBlendAttachmentState* blend, uint8_t mask = 0xF);
    VkPipeline EnsureBlitPipeline(VkFormat rtv, bool over = false);
    bool DepthCopy(const State& s);
    VkPipeline EnsureDepthCopyPipeline();
    void DrawDepthCopy(VTarget& src, VTarget& dst, VkPipeline pso);
    bool AssembleDraw(const State& s, uint32_t prim, uint32_t start, uint32_t count, const uint8_t* indices,
                      Assembled& a);
    bool BindTargets(const State& s);
    void EnsureVertexShader(const State& s, const Assembled& a, DrawShaders& sh);
    bool BindStageTexturesAndTargets(const State& s, DrawTextures& stages);
    Image* BindTexture(const State& s, int stage, DrawTextures& stages);
    Image* BindTargetTexture(const State& s, int stage, const TextureDesc& d, VTarget& t, bool box,
                             DrawTextures& stages);
    Image* BindGuestTexture(const State& s, int stage, const TextureDesc& d, const FormatInfo& fi,
                            DrawTextures& stages);
    bool CreateTexture(Tex& tx, const TextureDesc& d, const FormatInfo& fi, uint64_t key);
    void UploadTexture(Tex& tx, const TextureDesc& d, const uint8_t* p, const uint32_t* colours);
    void UploadTextureLevel(Image& img, uint32_t f, uint32_t l, const std::vector<uint8_t>& px, uint32_t pitch);
    void LatchPackKey(Tex& tx, const TextureDesc& d, const FormatInfo& fi, const uint32_t* colours);
    void LatchPackImage(Tex& tx);
    void CreatePackTexture(Tex& r, const texturedumper::Image& img);
    void ReleasePackTextures();
    void EnsurePixelShader(const PixelKey& pk, DrawShaders& sh);
    void BuildPipelineKey(const State& s, const Assembled& a, uint64_t vh, uint64_t ph, PsoKey& k);
    void KeyDepthStencil(const State& s, const Assembled& a, PsoKey& k);
    VkPipeline EnsurePipeline(const PsoKey& k, VkShaderModule vs, VkShaderModule ps);
    VkPipeline MakePipeline(const PsoKey& k, VkShaderModule vs, VkShaderModule ps, VkShaderModule gs);
    float TargetScale() const;
    void FillWideConstants(DrawConstants& c) const;
    void MeasureDrawX(const Assembled& a, bool writes_depth);
    float draw_y_[2] = {1, 0};
    float draw_x_[2] = {1, 0};
    void ChooseDrawSide(bool writes_depth);
    bool DrawIs2D(const State& s, const Assembled& a, bool writes_depth) const;
    float draw_side_ = 0, draw_vside_ = 0;
    float draw_dx_ = 0, draw_dy_ = 0;
    bool HudPlacing() const;
    bool draw_sided_ = false;
    bool draw_ui_ = false;
    struct SidedPiece {
        float lo, hi, top, bottom, side, vside, dx, dy;
    };
    std::vector<SidedPiece> sided_;
    std::vector<SidedPiece> prev_sided_;
    bool PresentPost(VTarget& t, const float uv[4], const VkViewport& vp);
    bool EnsurePresentPipelines();
    bool EnsureMid(uint32_t w, uint32_t h);
    bool EnsureColourLut();
    ImageRef clut_;
    uint32_t clut_n_ = 0;
    std::string clut_name_;
    bool CaptureDue();
    void PresentSwap(uint32_t bi);
    PerfClock::time_point last_swap_{};
    bool SkippedByTexture(const State& s);
    void WriteFrameTime();
    size_t capture_at_next_ = 0;
    PerfClock::time_point run_start_ = PerfClock::now();
    PerfClock::time_point last_present_{};
    FILE* frame_times_ = nullptr;
    uint64_t frame_counts_[6] = {};  // g_perf's counts at the last frame time line
    VkPipeline easu_pso_ = VK_NULL_HANDLE, final_pso_ = VK_NULL_HANDLE;
    bool present_failed_ = false;
    bool ReadShown(uint32_t bi, std::vector<uint8_t>& px);
    ImageRef mid_;
    float wide_ = 1.0f;
    float RenderTargetShift(const State& s, const DrawShaders& sh) const;
    void FillDepthConstants(const State& s, const DrawShaders& sh, const PsoKey& k, DrawConstants& c) const;
    void UploadConstants(const State& s, const DrawShaders& sh, const PsoKey& k, const DrawTextures& stages,
                         GuestExtent size, DrawConstants& c, Descriptors& set);
    void FillTextureConstants(const State& s, const DrawShaders& sh, const DrawTextures& stages, DrawConstants& c);
    void FillTexelConstants(int i, const TextureDesc& d, const DrawTextures& stages, DrawConstants& c);
    void CaptureDraw(const State& s, uint32_t prim, uint32_t count, const PsoKey& k, const DrawConstants& c,
                     const DrawTextures& stages);
    void CaptureTextures(const State& s, const DrawTextures& stages);
    void EndDrawCapture(const State& s, const Assembled& a, uint32_t count, const DrawConstants& c);
    void CaptureInputs(const State& s, const Assembled& a, const DrawConstants& c);
    void CaptureShaders(const State& s, const Assembled& a, const DrawShaders& sh, const PixelKey& pk);
    void CaptureShader(const char* kind, uint64_t key, const std::string& src);
    void SetTextureViewsAndSamplers(const State& s, const Assembled& a, DrawTextures& stages, Descriptors& set);
    VkSampler StageSampler(const State& s, int i, const DrawTextures& stages, bool clamp, bool fixed, uint32_t aniso);
    void SetViewportScissor(const State& s, uint32_t gw);
    void SetBlendFactorStencilRef(const State& s);
    void UpdateTargetUsed(uint32_t data);
    void DrawGeometry(const Assembled& a);
    bool EnsureQueries();
    uint32_t BeginCountedDraw(const State& s);
    void EndCountedDraw(uint32_t query);
    float SamplesPerPixel() const;
    void DeliverReports();
    void FinishCapture(const Surface& front);
    void StartCapture();
    void ResizeSwap();
    void UploadGammaRamp();
    void DrawFrame(VTarget* t, uint32_t bi);
    void BlitFrame(VTarget& t);
    void RenderUi();
    void ReleaseOldTextures();
    void ResetFrame();
    void CountFps();
    void ApplySettings();
    ImageRef Shrink(Image& src, uint32_t w, uint32_t h, const ShrinkOptions& o = {});
    bool ReadBack(Image& img, uint32_t w, uint32_t h, std::vector<uint8_t>& out);
    void CapStep(const Surface& rt);
    void CapSave(Image& img, uint32_t w, uint32_t h, float mul, const char* name);
    void CapSave(VTarget& t, const char* name);
    void CapShown(uint32_t bi);
    void Screenshot(uint32_t bi);
    void StartPipelineWorkers();
    void ListPipeline(uint64_t h, const PsoKey& k);
    std::filesystem::path shown_dir_;
    std::unordered_set<uint32_t> cap_vs_;
    struct TargetSlot {
        uint32_t data;
        uint64_t key;
        uint32_t gw;
        uint32_t gh;
        std::unordered_map<uint64_t, VTarget>::iterator it;
        bool sized;
    };
    VTarget* FindTarget(const Surface& s, bool depth);
    VTarget* EnsureTarget(const Surface& s, bool depth);
    TargetSlot FindTargetSlot(const Surface& s, bool depth);
    uint64_t SizedTargetKey(uint32_t data, bool depth, uint32_t gw, uint32_t gh) const;
    bool TargetStale(uint32_t data) const;
    void ReleaseTarget(std::unordered_map<uint64_t, VTarget>::iterator it);
    bool CreateTargetResource(VTarget& t, const Surface& s);
    void UploadTarget(VTarget& t, const Surface& s);
    ImageRef CreatePixelTexture(const std::vector<uint8_t>& px, uint32_t pitch, uint32_t w, uint32_t h);
    void ClearImage(Image& i, const std::vector<VkRect2D>& rects, const VkClearValue& v);
    struct ClearArgs {
        uint32_t count;
        const uint8_t* rects;
        uint32_t flags;
        uint32_t color;
        float z;
        uint32_t stencil;
    };
    void ClearTarget(VTarget& t, const State& s, const ClearArgs& c, std::vector<VkRect2D>& rs);
    void ClearChannels(VTarget& t, const float col[4], uint8_t mask, const std::vector<VkRect2D>& rs);
    VkPipeline EnsureClearPipeline(VkFormat rtv, uint8_t mask);
    struct CopyList {
        uint32_t count;
        uint32_t src_rects;
        uint32_t dst_points;
        float kx = 1, ky = 1;
    };
    void CopyByDraw(const VTarget& src_t, VTarget& dst_t, const CopyList& l);
    void CopyByRegion(const VTarget& src_t, VTarget& dst_t, const CopyList& l);
    struct CopySource {
        Image* img;
        ImageRef down;
    };
    CopySource PrepareCopySource(VTarget& t, ImageRef reuse);
    struct Late;
    void ResetLate(Late& l, const VTarget& t);
    void RecordLateCopy(Late& l, uint32_t slot, Image& src);

    // Vulkan objects
    VkInstance instance_ = VK_NULL_HANDLE;
    VkPhysicalDevice phys_ = VK_NULL_HANDLE;
    VkDevice dev_ = VK_NULL_HANDLE;
    uint32_t family_ = 0;
    VkQueue queue_ = VK_NULL_HANDLE;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkSwapchainKHR swap_ = VK_NULL_HANDLE;
    VkFormat swap_fmt_ = kBgra;
    VkPresentModeKHR swap_mode_ = VK_PRESENT_MODE_FIFO_KHR;
    std::vector<ImageRef> bb_;
    uint32_t present_image_ = 0;  // the acquired swap chain image
    bool swap_lost_ = false;      // out of date or minimised: made again at the next present
    bool pending_dummies_ = false;
    std::vector<VkSemaphore> rendered_;  // per swap chain image: the frame's work done, the present waits
    VkSemaphore pending_acquire_ = VK_NULL_HANDLE;  // the next submit waits for the acquired image
    VkSemaphore timeline_ = VK_NULL_HANDLE;
    uint64_t timeline_value_ = 0;
    VkPhysicalDeviceProperties props_{};
    VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkPipelineCache pcache_ = VK_NULL_HANDLE;
    VkDescriptorPool ui_pool_ = VK_NULL_HANDLE;
    bool ui_ready_ = false;
    Frame frames_[kFrames];
    uint64_t frame_ = 0;
    VkCommandBuffer cb_ = VK_NULL_HANDLE;
    // rendering: the attachments the next draw renders to, and the ones a vkCmdBeginRendering has open
    Attach want_;
    Attach active_;
    bool rendering_ = false;
    uint32_t render_w_ = 0, render_h_ = 0;
    decltype(State::vsc) last_vsc_{};
    Alloc last_vsc_alloc_{};
    DrawConstants last_dc_{};
    Alloc last_dc_alloc_{};
    std::unordered_map<uint64_t, VkShaderModule> modules_;  // by source hash, null when it did not compile
    std::unordered_map<uint64_t, uint64_t> shader_src_;
    std::unordered_map<uint64_t, VkPipeline> psos_;
    std::mutex pre_mutex_;
    std::unordered_map<uint64_t, VkPipeline> pre_psos_;
    std::mutex module_mutex_;  // the pipeline workers make modules too
    FILE* pso_file_ = nullptr;
    std::unordered_set<uint64_t> pso_listed_;
    std::unordered_map<uint64_t, VkSampler> samplers_;
    std::unordered_map<uint64_t, VTarget> targets_;
    std::unordered_map<uint32_t, uint64_t> target_used_;
    std::array<UsedTarget, 4> recent_used_{};
    uint32_t recent_used_next_ = 0;
    DrawBinds binds_;
    std::unordered_map<uint64_t, Tex> textures_;
    std::unordered_map<uint64_t, Tex> pack_textures_;
    std::unordered_map<uint64_t, VkPipeline> blit_psos_;
    ImageRef dummy_[3];  // 2D, 3D, cube: a stage without a texture
    FILE* cap_ = nullptr;
    std::filesystem::path cap_dir_;
    uint32_t cap_i_ = 0;
    uint32_t cap_rt_ = 0;
    std::unordered_set<uint32_t> cap_seen_;
    std::unordered_set<uint64_t> cap_sh_;
    struct CapTex {
        Image* img;
        ImageRef keep;
        uint32_t data;
        uint32_t w;
        uint32_t h;
    };
    std::vector<CapTex> cap_tex_;
    ImageRef lut_;
    uint8_t ramp_[768] = {};
    bool ramp_dirty_ = false;
    bool gamma_ = false;
    VTarget* bound_rt_ = nullptr;
    VTarget* bound_ds_ = nullptr;
    VTarget* draw_rt_ = nullptr;
    VTarget* draw_ds_ = nullptr;
    uint32_t guest_w_ = 640;
    uint32_t guest_h_ = 480;
    uint32_t bb_w_ = 0;
    uint32_t bb_h_ = 0;
    float scale_ = 1.0f;
    struct Late {
        BufferRef rb[kFrames];
        ImageRef down;
        uint64_t fence[kFrames] = {};
        uint64_t frame[kFrames] = {};
        uint64_t delivered = 0;
        uint32_t gw = 0;
        uint32_t gh = 0;
        uint32_t next = 0;
    };
    std::unordered_map<uint32_t, Late> late_;
    std::mutex late_lock_;
    std::atomic<uint64_t> presents_seen_{0};
    std::atomic<uint64_t> present_fence_[kFrames + 1] = {};
    Assembled asm_;
    struct Query {
        uint32_t index;
        float samples_per_pixel;
    };
    struct Report {
        uint32_t va;
        std::vector<Query> queries;
        uint64_t fence;
    };
    VkQueryPool query_pool_ = VK_NULL_HANDLE;
    // per frame slot, two timestamps: the frame's first command and its present's; read once the slot comes round
    VkQueryPool ts_pool_ = VK_NULL_HANDLE;
    bool ts_written_[kFrames] = {};
    void FrameTimestamp(bool end);
    bool queries_failed_ = false;
    uint32_t query_next_ = 0;
    std::vector<Query> counted_;
    std::deque<Report> reports_;
    uint64_t presents_ = 0;
    double fps_ = 0;
    std::chrono::steady_clock::time_point fps_t_ = std::chrono::steady_clock::now();
    uint64_t fps_n_ = 0;
};

// Init

bool VulkanBackend::Init(uint32_t width, uint32_t height) {
    g_reversed_depth = true;
    guest_w_ = width;
    guest_h_ = height;
    if (!CreateInstance() || !CreateDevice()) return false;
    if (!CreateLayout()) return false;
    CreateFrames();
    if (!CreateSwapchain()) return false;
    CreateDummies();
    lut_ = MakeImage(kBgra, 256, 1, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    StartPipelineWorkers();
    LoadSettings();
    Begin();
    CreateUi();
    // the gamma lookup starts as the identity
    for (int i = 0; i < 256; i++) ramp_[i] = ramp_[256 + i] = ramp_[512 + i] = uint8_t(i);
    ramp_dirty_ = true;
    LOGI("vulkan: %ux%u guest, %ux%u window, render scale %.3f", width, height, bb_w_, bb_h_, scale_);
    return true;
}

bool VulkanBackend::CreateInstance() {
    if (volkInitialize() != VK_SUCCESS) {
        LOGE("vulkan: no Vulkan loader on this system");
        return false;
    }
    std::vector<const char*> ext = SurfaceExtensions();
    if (ext.empty()) return false;
    std::vector<const char*> layers;
    if (std::getenv("XBE_VULKAN_DEBUG")) layers.push_back("VK_LAYER_KHRONOS_validation");
    VkApplicationInfo ai{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    std::string name = platform::Utf8(app::kGame.title);
    ai.pApplicationName = name.c_str();
    ai.pEngineName = "XboxXBERecomp";
    ai.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ci.pApplicationInfo = &ai;
    ci.enabledExtensionCount = uint32_t(ext.size());
    ci.ppEnabledExtensionNames = ext.data();
    ci.enabledLayerCount = uint32_t(layers.size());
    ci.ppEnabledLayerNames = layers.data();
    if (vkCreateInstance(&ci, nullptr, &instance_) != VK_SUCCESS) {
        LOGE("vulkan: no Vulkan 1.3 instance");
        return false;
    }
    volkLoadInstance(instance_);
    if (!CreateSurface(instance_, &surface_)) {
        LOGE("vulkan: no surface for the window");
        return false;
    }
    return true;
}

bool VulkanBackend::CreateDevice() {
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(instance_, &n, nullptr);
    std::vector<VkPhysicalDevice> devs(n);
    vkEnumeratePhysicalDevices(instance_, &n, devs.data());
    // a discrete GPU first, as on hybrid laptops; Vulkan 1.3 with a queue that draws and presents
    int best = -1, best_rank = -1;
    for (int i = 0; i < int(n); i++) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(devs[size_t(i)], &p);
        if (p.apiVersion < VK_API_VERSION_1_3) continue;
        int rank = p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 2 : p.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 1 : 0;
        if (rank > best_rank) best = i, best_rank = rank;
    }
    if (best < 0) {
        LOGE("vulkan: no Vulkan 1.3 GPU");
        return false;
    }
    phys_ = devs[size_t(best)];
    vkGetPhysicalDeviceProperties(phys_, &props_);
    uint32_t qn = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(phys_, &qn, nullptr);
    std::vector<VkQueueFamilyProperties> qf(qn);
    vkGetPhysicalDeviceQueueFamilyProperties(phys_, &qn, qf.data());
    bool found = false;
    for (uint32_t i = 0; i < qn && !found; i++) {
        VkBool32 present = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(phys_, i, surface_, &present);
        if ((qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present) family_ = i, found = true;
    }
    if (!found) {
        LOGE("vulkan: no queue that draws to the window");
        return false;
    }
    LOGR("Vulkan device: %s (Vulkan %u.%u.%u, driver %u)", props_.deviceName, VK_API_VERSION_MAJOR(props_.apiVersion),
         VK_API_VERSION_MINOR(props_.apiVersion), VK_API_VERSION_PATCH(props_.apiVersion), props_.driverVersion);
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qi.queueFamilyIndex = family_;
    qi.queueCount = 1;
    qi.pQueuePriorities = &prio;
    const char* ext[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME, VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME,
                         VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME};
    VkPhysicalDeviceFeatures supported{};
    vkGetPhysicalDeviceFeatures(phys_, &supported);
    VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    f13.dynamicRendering = VK_TRUE;
    f13.synchronization2 = VK_TRUE;
    VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    f12.pNext = &f13;
    f12.timelineSemaphore = VK_TRUE;
    f12.hostQueryReset = VK_TRUE;
    VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    f2.pNext = &f12;
    f2.features.geometryShader = supported.geometryShader;
    f2.features.samplerAnisotropy = supported.samplerAnisotropy;
    f2.features.fillModeNonSolid = supported.fillModeNonSolid;
    f2.features.textureCompressionBC = supported.textureCompressionBC;
    f2.features.occlusionQueryPrecise = supported.occlusionQueryPrecise;
    f2.features.depthBiasClamp = supported.depthBiasClamp;
    VkDeviceCreateInfo ci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    ci.pNext = &f2;
    ci.queueCreateInfoCount = 1;
    ci.pQueueCreateInfos = &qi;
    ci.enabledExtensionCount = uint32_t(std::size(ext));
    ci.ppEnabledExtensionNames = ext;
    if (vkCreateDevice(phys_, &ci, nullptr, &dev_) != VK_SUCCESS) {
        LOGE("vulkan: no device (Vulkan 1.3 with push descriptors and dynamic rendering)");
        return false;
    }
    volkLoadDevice(dev_);
    g_dev = dev_;
    vkGetDeviceQueue(dev_, family_, 0, &queue_);
    VmaVulkanFunctions vf{};
    vf.vkGetInstanceProcAddr = vkGetInstanceProcAddr;
    vf.vkGetDeviceProcAddr = vkGetDeviceProcAddr;
    VmaAllocatorCreateInfo ai{};
    ai.vulkanApiVersion = VK_API_VERSION_1_3;
    ai.physicalDevice = phys_;
    ai.device = dev_;
    ai.instance = instance_;
    ai.pVulkanFunctions = &vf;
    if (vmaCreateAllocator(&ai, &g_vma) != VK_SUCCESS) return false;
    VkSemaphoreTypeCreateInfo tci{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
    tci.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    sci.pNext = &tci;
    vkCreateSemaphore(dev_, &sci, nullptr, &timeline_);
    VkPipelineCacheCreateInfo pci{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
    vkCreatePipelineCache(dev_, &pci, nullptr, &pcache_);
    if (props_.limits.timestampComputeAndGraphics) {
        VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
        qi.queryCount = 2 * kFrames;
        vkCreateQueryPool(dev_, &qi, nullptr, &ts_pool_);
    }
    return true;
}

// app::VSync to a present mode: On and Adaptive wait for the refresh (Adaptive late frames go at once where the
// display has FIFO_RELAXED), Fast shows the newest finished frame (MAILBOX), Off and VRR do not wait
VkPresentModeKHR WantedMode(const std::vector<VkPresentModeKHR>& have) {
    auto has = [&](VkPresentModeKHR m) { return std::find(have.begin(), have.end(), m) != have.end(); };
    switch (app::Get().vsync_mode) {
        case app::VSync::Off:
        case app::VSync::Vrr:
            if (has(VK_PRESENT_MODE_IMMEDIATE_KHR)) return VK_PRESENT_MODE_IMMEDIATE_KHR;
            if (has(VK_PRESENT_MODE_MAILBOX_KHR)) return VK_PRESENT_MODE_MAILBOX_KHR;
            break;
        case app::VSync::Fast:
            if (has(VK_PRESENT_MODE_MAILBOX_KHR)) return VK_PRESENT_MODE_MAILBOX_KHR;
            break;
        case app::VSync::Adaptive:
            if (has(VK_PRESENT_MODE_FIFO_RELAXED_KHR)) return VK_PRESENT_MODE_FIFO_RELAXED_KHR;
            break;
        default:
            break;
    }
    return VK_PRESENT_MODE_FIFO_KHR;
}

bool VulkanBackend::CreateSwapchain() {
    int pw = 0, ph = 0;
    WindowPixels(pw, ph);
    VkSurfaceCapabilitiesKHR caps{};
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(phys_, surface_, &caps);
    VkExtent2D extent = caps.currentExtent;
    if (extent.width == UINT32_MAX) extent = {uint32_t(std::max(pw, 1)), uint32_t(std::max(ph, 1))};
    extent.width = std::clamp(extent.width, caps.minImageExtent.width, caps.maxImageExtent.width);
    extent.height = std::clamp(extent.height, caps.minImageExtent.height, caps.maxImageExtent.height);
    if (!extent.width || !extent.height) return false;  // minimised
    uint32_t n = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(phys_, surface_, &n, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(n);
    vkGetPhysicalDeviceSurfaceFormatsKHR(phys_, surface_, &n, formats.data());
    VkSurfaceFormatKHR fmt = formats.empty() ? VkSurfaceFormatKHR{kBgra, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR} : formats[0];
    for (auto& f : formats)
        if ((f.format == kBgra || f.format == VK_FORMAT_R8G8B8A8_UNORM) && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            fmt = f;
            if (f.format == kBgra) break;
        }
    vkGetPhysicalDeviceSurfacePresentModesKHR(phys_, surface_, &n, nullptr);
    std::vector<VkPresentModeKHR> modes(n);
    vkGetPhysicalDeviceSurfacePresentModesKHR(phys_, surface_, &n, modes.data());
    swap_mode_ = WantedMode(modes);
    VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    ci.surface = surface_;
    ci.minImageCount = std::max(caps.minImageCount, uint32_t(kFrames));
    if (caps.maxImageCount) ci.minImageCount = std::min(ci.minImageCount, caps.maxImageCount);
    ci.imageFormat = fmt.format;
    ci.imageColorSpace = fmt.colorSpace;
    ci.imageExtent = extent;
    ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.preTransform = caps.currentTransform;
    ci.compositeAlpha = (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR)
                            ? VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR
                            : VkCompositeAlphaFlagBitsKHR(caps.supportedCompositeAlpha & -caps.supportedCompositeAlpha);
    ci.presentMode = swap_mode_;
    ci.clipped = VK_TRUE;
    ci.oldSwapchain = swap_;
    VkSwapchainKHR made = VK_NULL_HANDLE;
    if (vkCreateSwapchainKHR(dev_, &ci, nullptr, &made) != VK_SUCCESS) {
        LOGE("vulkan: no swap chain");
        return false;
    }
    DestroySwapchain();
    swap_ = made;
    swap_fmt_ = fmt.format;
    vkGetSwapchainImagesKHR(dev_, swap_, &n, nullptr);
    std::vector<VkImage> imgs(n);
    vkGetSwapchainImagesKHR(dev_, swap_, &n, imgs.data());
    for (VkImage i : imgs) {
        auto r = std::make_shared<Image>();
        r->fmt = swap_fmt_;
        r->w = extent.width;
        r->h = extent.height;
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = i;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = swap_fmt_;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCreateImageView(dev_, &vi, nullptr, &r->att);
        r->img = i;
        r->owned = false;
        bb_.push_back(r);
        VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VkSemaphore s;
        vkCreateSemaphore(dev_, &sci, nullptr, &s);
        rendered_.push_back(s);
    }
    bb_w_ = extent.width;
    bb_h_ = extent.height;
    app::SetPresentSize(bb_w_, bb_h_);
    LOGI("vulkan: swap chain %ux%u, %u images, present mode %d", bb_w_, bb_h_, n, int(swap_mode_));
    return true;
}

void VulkanBackend::DestroySwapchain() {
    if (!swap_) return;
    vkDeviceWaitIdle(dev_);
    bb_.clear();
    for (VkSemaphore s : rendered_) vkDestroySemaphore(dev_, s, nullptr);
    rendered_.clear();
    vkDestroySwapchainKHR(dev_, swap_, nullptr);
    swap_ = VK_NULL_HANDLE;
}

// one push descriptor set laid out as the D3D12 root signature: b0 b1, t0..t3, s0..s3
bool VulkanBackend::CreateLayout() {
    VkDescriptorSetLayoutBinding b[spirv::kBindings]{};
    for (uint32_t i = 0; i < spirv::kBindings; i++) {
        b[i].binding = i;
        b[i].descriptorCount = 1;
        b[i].stageFlags = VK_SHADER_STAGE_ALL_GRAPHICS;
        b[i].descriptorType = i < spirv::kBindingTexture   ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER
                              : i < spirv::kBindingSampler ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE
                                                           : VK_DESCRIPTOR_TYPE_SAMPLER;
    }
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
    li.bindingCount = spirv::kBindings;
    li.pBindings = b;
    if (vkCreateDescriptorSetLayout(dev_, &li, nullptr, &set_layout_) != VK_SUCCESS) return false;
    VkPipelineLayoutCreateInfo pi{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pi.setLayoutCount = 1;
    pi.pSetLayouts = &set_layout_;
    return vkCreatePipelineLayout(dev_, &pi, nullptr, &layout_) == VK_SUCCESS;
}

void VulkanBackend::CreateFrames() {
    for (auto& f : frames_) {
        VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pi.queueFamilyIndex = family_;
        vkCreateCommandPool(dev_, &pi, nullptr, &f.pool);
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool = f.pool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        vkAllocateCommandBuffers(dev_, &ai, &f.cb);
        f.upload = MakeBuffer(kUploadSize,
                              VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                                  VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                              false);
        VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        vkCreateSemaphore(dev_, &sci, nullptr, &f.acquired);
    }
}

// a 2D, a 3D and a cube texture of one transparent black texel for stages without one, as D3D12's null views read
void VulkanBackend::CreateDummies() {
    dummy_[0] = MakeImage(kBgra, 1, 1, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    dummy_[1] = MakeImage(kBgra, 1, 1, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, false, 1, 2);
    dummy_[2] = MakeImage(kBgra, 1, 1, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, false, 1, 1, true);
    pending_dummies_ = true;
}

void VulkanBackend::CreateUi() {
    VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 16};
    VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pi.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    pi.maxSets = 16;
    pi.poolSizeCount = 1;
    pi.pPoolSizes = &size;
    vkCreateDescriptorPool(dev_, &pi, nullptr, &ui_pool_);
    ImGui_ImplVulkan_InitInfo ii{};
    ii.Instance = instance_;
    ii.PhysicalDevice = phys_;
    ii.Device = dev_;
    ii.QueueFamily = family_;
    ii.Queue = queue_;
    ii.DescriptorPool = ui_pool_;
    ii.MinImageCount = 2;
    ii.ImageCount = uint32_t(std::max<size_t>(bb_.size(), 2));
    ii.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    ii.PipelineCache = pcache_;
    ii.UseDynamicRendering = true;
    ii.PipelineRenderingCreateInfo = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    ii.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
    ii.PipelineRenderingCreateInfo.pColorAttachmentFormats = &swap_fmt_;
    std::lock_guard lock(app::UiLock());
    ui_ready_ = ImGui::GetCurrentContext() && ImGui_ImplVulkan_Init(&ii);
}

void VulkanBackend::LoadSettings() {
    scale_ = app::RenderScale(guest_w_, guest_h_);
    wide_ = app::WideFactor();
    texturedumper::Update();
}

// Command recording

void VulkanBackend::Begin() {
    Frame& f = CurrentFrame();
    cb_ = f.cb;
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cb_, &bi);
    bound_rt_ = bound_ds_ = nullptr;
    want_ = active_ = {};
    rendering_ = false;
    binds_ = {};
    if (pending_dummies_) {
        pending_dummies_ = false;
        for (auto& d : dummy_) {
            Transition(*d, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            VkClearColorValue black{};
            VkImageSubresourceRange r{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, d->layers};
            vkCmdClearColorImage(cb_, d->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &r);
            Transition(*d, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        }
    }
}

uint64_t VulkanBackend::Completed() {
    uint64_t v = 0;
    vkGetSemaphoreCounterValue(dev_, timeline_, &v);
    return v;
}

void VulkanBackend::WaitTimeline(uint64_t value) {
    VkSemaphoreWaitInfo wi{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
    wi.semaphoreCount = 1;
    wi.pSemaphores = &timeline_;
    wi.pValues = &value;
    vkWaitSemaphores(dev_, &wi, UINT64_MAX);
}

// present: the frame's last submit, which signals the acquired image's rendered_ semaphore for the present to wait on
void VulkanBackend::Submit(bool wait, bool present) {
    EndRendering();
    last_vsc_alloc_ = {};
    last_dc_alloc_ = {};
    vkEndCommandBuffer(cb_);
    uint64_t value = ++timeline_value_;
    VkSemaphoreSubmitInfo signals[2]{};
    signals[0] = {VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, nullptr, timeline_, value,
                  VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0};
    uint32_t nsignal = 1;
    if (present) {
        signals[1] = {VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, nullptr, rendered_[present_image_], 0,
                      VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0};
        nsignal = 2;
    }
    VkSemaphoreSubmitInfo waitsem{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, nullptr, pending_acquire_, 0,
                                  VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0};
    VkCommandBufferSubmitInfo cbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, nullptr, cb_, 0};
    VkSubmitInfo2 si{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    si.waitSemaphoreInfoCount = pending_acquire_ ? 1 : 0;
    si.pWaitSemaphoreInfos = &waitsem;
    si.commandBufferInfoCount = 1;
    si.pCommandBufferInfos = &cbi;
    si.signalSemaphoreInfoCount = nsignal;
    si.pSignalSemaphoreInfos = signals;
    if (vkQueueSubmit2(queue_, 1, &si, VK_NULL_HANDLE) != VK_SUCCESS) LOG_FIRST(4, "vulkan: submit failed");
    pending_acquire_ = VK_NULL_HANDLE;
    Frame& f = CurrentFrame();
    f.fence = value;
    if (wait) {
        WaitTimeline(value);
        vkResetCommandPool(dev_, f.pool, 0);
        f.garbage.clear();
        f.garbage_buffers.clear();
        f.used = 0;
        Begin();
    }
}

void VulkanBackend::Flush() {
    Submit(true);
    DeliverReports();
}

VulkanBackend::Alloc VulkanBackend::Upload(uint64_t size, uint64_t align) {
    Frame& f = CurrentFrame();
    g_perf.upload += size;
    uint64_t o = (f.used + align - 1) & ~(align - 1);
    if (o + size <= kUploadSize) {
        f.used = o + size;
        return {f.upload->buf, o, f.upload->map + o};
    }
    // too big for the ring: a buffer of its own, released with the frame
    g_perf.overflow++;
    BufferRef r = MakeBuffer(size, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                                       VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                             false);
    f.garbage_buffers.push_back(r);
    return {r->buf, 0, r->map};
}

// one barrier per layout change, everything before it against everything after: correct first, and the console's
// own frames rarely go past a few hundred
void VulkanBackend::Transition(Image& i, VkImageLayout want) {
    if (i.layout == want && want != VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) return;
    EndRendering();
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
    b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    b.oldLayout = i.layout;
    b.newLayout = want;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = i.img;
    b.subresourceRange = {Aspects(i), 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
    VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    di.imageMemoryBarrierCount = 1;
    di.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cb_, &di);
    i.layout = want;
}

void VulkanBackend::EndRendering() {
    if (!rendering_) return;
    vkCmdEndRendering(cb_);
    rendering_ = false;
}

// the next draw's attachments open for rendering, kept open across draws that share them
void VulkanBackend::EnsureRendering() {
    if (rendering_ && active_ == want_) return;
    EndRendering();
    if (want_.color) Transition(*want_.color, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    if (want_.depth) Transition(*want_.depth, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
    Image* any = want_.color ? want_.color : want_.depth;
    if (!any) return;
    VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    mb.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
    mb.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    mb.dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                      VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
    mb.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT |
                       VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    di.memoryBarrierCount = 1;
    di.pMemoryBarriers = &mb;
    vkCmdPipelineBarrier2(cb_, &di);
    VkRenderingAttachmentInfo ca{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    VkRenderingAttachmentInfo da{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    if (want_.color) {
        ca.imageView = want_.color->att;
        ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        ca.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    }
    if (want_.depth) {
        da.imageView = want_.depth->att;
        da.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        da.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        da.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    }
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, {any->w, any->h}};
    ri.layerCount = 1;
    ri.colorAttachmentCount = want_.color ? 1 : 0;
    ri.pColorAttachments = &ca;
    ri.pDepthAttachment = want_.depth ? &da : nullptr;
    ri.pStencilAttachment = want_.depth ? &da : nullptr;
    vkCmdBeginRendering(cb_, &ri);
    rendering_ = true;
    active_ = want_;
    render_w_ = any->w;
    render_h_ = any->h;
}

// D3D's viewport: y down, the shaders' clip space as D3D has it
VkViewport D3dViewport(float x, float y, float w, float h) { return {x, y + h, w, -h, 0, 1}; }

void VulkanBackend::SetFullViewport(uint32_t w, uint32_t h) {
    VkViewport vp = D3dViewport(0, 0, float(w), float(h));
    VkRect2D sc{{0, 0}, {w, h}};
    vkCmdSetViewport(cb_, 0, 1, &vp);
    vkCmdSetScissor(cb_, 0, 1, &sc);
    binds_ = {};
}

void VulkanBackend::SetBlitTarget(Image& i) {
    want_ = {&i, nullptr};
    bound_rt_ = bound_ds_ = nullptr;
    SetFullViewport(i.w, i.h);
}

VkSampler VulkanBackend::EnsureSampler(const VkSamplerCreateInfo& ci) {
    uint64_t key = Hash(&ci.flags, sizeof(ci) - offsetof(VkSamplerCreateInfo, flags));
    auto it = samplers_.find(key);
    if (it != samplers_.end()) return it->second;
    VkSampler s = VK_NULL_HANDLE;
    vkCreateSampler(dev_, &ci, nullptr, &s);
    samplers_[key] = s;
    return s;
}

// four clamp samplers: linear at slot 0, point at slots 1 to 3
void VulkanBackend::BlitSamplers(VkSampler out[4]) {
    for (int i = 0; i < 4; i++) {
        VkSamplerCreateInfo ci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        ci.magFilter = ci.minFilter = i == 0 ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
        ci.mipmapMode = i == 0 ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
        ci.addressModeU = ci.addressModeV = ci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        ci.maxLod = VK_LOD_CLAMP_NONE;
        out[i] = EnsureSampler(ci);
    }
}

void VulkanBackend::PushDescriptors(const Descriptors& d) {
    if (binds_.descriptors && binds_.set == d) return;
    VkDescriptorBufferInfo bi[2];
    VkDescriptorImageInfo ii[4], si[4];
    VkWriteDescriptorSet w[spirv::kBindings]{};
    for (uint32_t i = 0; i < spirv::kBindings; i++) {
        w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[i].dstBinding = i;
        w[i].descriptorCount = 1;
    }
    for (int i = 0; i < 2; i++) {
        bi[i] = {d.ubo[i], d.off[i], d.range[i]};
        w[i].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        w[i].pBufferInfo = &bi[i];
    }
    for (int i = 0; i < 4; i++) {
        ii[i] = {VK_NULL_HANDLE, d.view[i], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        w[spirv::kBindingTexture + i].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        w[spirv::kBindingTexture + i].pImageInfo = &ii[i];
        si[i] = {d.smp[i], VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED};
        w[spirv::kBindingSampler + i].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
        w[spirv::kBindingSampler + i].pImageInfo = &si[i];
    }
    vkCmdPushDescriptorSetKHR(cb_, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, spirv::kBindings, w);
    binds_.descriptors = true;
    binds_.set = d;
}

// the full screen triangle: cb at b0 and b1, views t0..t3 (the 2D dummy where null), the blit samplers unless given
void VulkanBackend::DrawFullScreen(VkPipeline pso, const Alloc& cb, Image* const views[4], const VkSampler* samplers) {
    if (!pso) return;
    EnsureRendering();
    Descriptors d;
    for (int i = 0; i < 2; i++) {
        d.ubo[i] = cb.buf;
        d.off[i] = cb.off;
        d.range[i] = 256;
    }
    for (int i = 0; i < 4; i++) d.view[i] = views[i] ? views[i]->view : dummy_[0]->view;
    if (samplers) std::memcpy(d.smp, samplers, sizeof(d.smp));
    else BlitSamplers(d.smp);
    binds_.pso = VK_NULL_HANDLE;
    vkCmdBindPipeline(cb_, VK_PIPELINE_BIND_POINT_GRAPHICS, pso);
    PushDescriptors(d);
    vkCmdDraw(cb_, 3, 1, 0, 0);
    binds_.pso = VK_NULL_HANDLE;
}

void VulkanBackend::Blit(Image& src, const float uvrect[4], const BlitOptions& o) {
    Transition(src, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    Transition(*lut_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    Alloc cb = Upload(256, 256);
    float c[8] = {uvrect[0], uvrect[1], uvrect[2], uvrect[3], o.gamma ? 1.0f : 0.0f, o.alpha ? 1.0f : 0.0f,
                  float(o.taps), o.mul};
    std::memcpy(cb.cpu, c, sizeof(c));
    Image* views[4] = {&src, lut_.get(), nullptr, nullptr};
    DrawFullScreen(EnsureBlitPipeline(o.rtv, o.over), cb, views);
}

VkShaderModule VulkanBackend::Module(const std::vector<uint32_t>& code) {
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = code.size() * 4;
    ci.pCode = code.data();
    VkShaderModule m = VK_NULL_HANDLE;
    vkCreateShaderModule(dev_, &ci, nullptr, &m);
    return m;
}

VkShaderModule VulkanBackend::Compile(const std::string& src, const char* entry, const char* profile, uint64_t& hash) {
    uint64_t profile_seed = Hash(profile, std::strlen(profile));
    uint64_t entry_seed = Hash(entry, std::strlen(entry), profile_seed);
    hash = Hash(src.data(), src.size(), entry_seed);
    std::lock_guard lock(module_mutex_);
    auto it = modules_.find(hash);
    if (it != modules_.end()) return it->second;
    if (const std::vector<uint32_t>* saved = g_shader_disk.Find(hash)) return modules_[hash] = Module(*saved);
    PerfScope perf_compile{g_perf.compile};
    g_perf.shaders++;
    std::vector<uint32_t> code;
    std::string error;
    if (!spirv::Compile(src, entry, profile, code, error)) {
        LOGE("vulkan: %s compile failed: %s", profile, error.c_str());
        LOGE("%s", src.c_str());
        return modules_[hash] = VK_NULL_HANDLE;
    }
    g_shader_disk.Add(hash, code);
    return modules_[hash] = Module(code);
}

VkPipelineColorBlendAttachmentState NoBlend(uint8_t mask = 0xF) {
    VkPipelineColorBlendAttachmentState b{};
    b.colorWriteMask = mask;
    return b;
}

// a full screen pass's pipeline: solid, no culling, one colour target (or depth alone), the given blend
VkPipeline VulkanBackend::FullScreenPipeline(VkShaderModule vs, VkShaderModule ps, VkFormat color, bool depth_only,
                                             const VkPipelineColorBlendAttachmentState* blend, uint8_t mask) {
    if (!vs || !ps) return VK_NULL_HANDLE;
    VkPipelineShaderStageCreateInfo st[2]{};
    st[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vs, "vs"};
    st[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, ps, "psmain"};
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.lineWidth = 1;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    if (depth_only) {
        ds.depthTestEnable = VK_TRUE;
        ds.depthWriteEnable = VK_TRUE;
        ds.depthCompareOp = VK_COMPARE_OP_ALWAYS;
    }
    VkPipelineColorBlendAttachmentState att = blend ? *blend : NoBlend(mask);
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = depth_only ? 0 : 1;
    cb.pAttachments = &att;
    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_BLEND_CONSTANTS};
    VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dy.dynamicStateCount = uint32_t(std::size(dyn));
    dy.pDynamicStates = dyn;
    VkPipelineRenderingCreateInfo ri{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    ri.colorAttachmentCount = depth_only ? 0 : 1;
    ri.pColorAttachmentFormats = &color;
    ri.depthAttachmentFormat = depth_only ? kDepthFormat : VK_FORMAT_UNDEFINED;
    ri.stencilAttachmentFormat = depth_only ? kDepthFormat : VK_FORMAT_UNDEFINED;
    VkGraphicsPipelineCreateInfo pi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pi.pNext = &ri;
    pi.stageCount = 2;
    pi.pStages = st;
    pi.pVertexInputState = &vi;
    pi.pInputAssemblyState = &ia;
    pi.pViewportState = &vp;
    pi.pRasterizationState = &rs;
    pi.pMultisampleState = &ms;
    pi.pDepthStencilState = &ds;
    pi.pColorBlendState = &cb;
    pi.pDynamicState = &dy;
    pi.layout = layout_;
    VkPipeline p = VK_NULL_HANDLE;
    if (vkCreateGraphicsPipelines(dev_, pcache_, 1, &pi, nullptr, &p) != VK_SUCCESS) {
        LOGE("vulkan: full screen pipeline failed");
        return VK_NULL_HANDLE;
    }
    return p;
}

VkPipeline VulkanBackend::EnsureBlitPipeline(VkFormat rtv, bool over) {
    auto& pso = blit_psos_[uint64_t(rtv) | uint64_t(over) << 32];
    if (pso) return pso;
    uint64_t h;
    VkShaderModule vs = Compile(kBlitHlsl, "vs", "vs_5_0", h);
    VkShaderModule ps = Compile(kBlitHlsl, "psmain", "ps_5_0", h);
    VkPipelineColorBlendAttachmentState b = NoBlend();
    if (over) {
        // premultiplied colour, the target keeps its alpha
        b.blendEnable = VK_TRUE;
        b.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
        b.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        b.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        b.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        b.colorBlendOp = b.alphaBlendOp = VK_BLEND_OP_ADD;
    }
    pso = FullScreenPipeline(vs, ps, rtv, false, &b);
    return pso;
}

// a copy of the target to read while drawing to it
Image* VulkanBackend::Feedback(VTarget& t) {
    if (!t.fb) {
        t.fb = MakeImage(t.img->fmt, t.w, t.h, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
        if (!t.fb) return nullptr;
    }
    Transition(*t.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    Transition(*t.fb, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageCopy c{};
    c.srcSubresource = c.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    c.extent = {t.w, t.h, 1};
    vkCmdCopyImage(cb_, t.img->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, t.fb->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   1, &c);
    Transition(*t.fb, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    return t.fb.get();
}

// Targets

void VulkanBackend::ReleaseTarget(std::unordered_map<uint64_t, VTarget>::iterator it) {
    if (bound_rt_ == &it->second || bound_ds_ == &it->second) bound_rt_ = bound_ds_ = nullptr;
    EndRendering();
    want_ = active_ = {};
    CurrentFrame().garbage.push_back(it->second.img);
    if (it->second.fb) CurrentFrame().garbage.push_back(it->second.fb);
    targets_.erase(it);
}

bool VulkanBackend::CreateTargetResource(VTarget& t, const Surface& s) {
    float sc = scale_;
    if (app::kGame.target_scale) {
        float want = app::kGame.target_scale(mem::VirtualFromPhysical(s.data), t.gw, t.gh, t.depth, scale_);
        if (want > 0) sc = want;
    }
    t.w = std::max(1u, uint32_t(std::lround(t.gw * sc)));
    t.h = std::max(1u, uint32_t(std::lround(t.gh * sc)));
    VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    usage |= t.depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    t.img = MakeImage(t.depth ? kDepthFormat : t.fmt, t.w, t.h, usage, t.depth);
    if (!t.img) return false;
    // cleared as D3D12's new targets are: black, or depth at the far end
    std::vector<VkRect2D> all{{{0, 0}, {t.w, t.h}}};
    VkClearValue v{};
    if (t.depth) v.depthStencil = {kDepthFar, 0};
    ClearImage(*t.img, all, v);
    return true;
}

uint64_t TargetMapKey(uint32_t data, bool depth) { return uint64_t(data) << 1 | (depth ? 1 : 0); }
constexpr int kKeyWidthShift = 34;
constexpr int kKeyHeightShift = 48;

uint64_t VulkanBackend::SizedTargetKey(uint32_t data, bool depth, uint32_t gw, uint32_t gh) const {
    uint64_t key = TargetMapKey(data, depth);
    bool small_depth = depth && (gw != guest_w_ || gh != guest_h_);
    if (!small_depth) return key;
    return key | uint64_t(gw) << kKeyWidthShift | uint64_t(gh) << kKeyHeightShift;
}

VulkanBackend::TargetSlot VulkanBackend::FindTargetSlot(const Surface& s, bool depth) {
    TargetSlot slot;
    slot.data = mem::VirtualFromPhysical(s.data);
    SurfaceSize(s, slot.gw, slot.gh);
    slot.key = SizedTargetKey(slot.data, depth, slot.gw, slot.gh);
    slot.it = targets_.find(slot.key);
    slot.sized = slot.it != targets_.end() && slot.it->second.gw == slot.gw && slot.it->second.gh == slot.gh;
    return slot;
}

VTarget* VulkanBackend::FindTarget(const Surface& s, bool depth) {
    if (!s.header) return nullptr;
    TargetSlot slot = FindTargetSlot(s, depth);
    if (!slot.sized) return nullptr;
    UpdateTargetUsed(slot.data);
    return &slot.it->second;
}

VTarget* VulkanBackend::EnsureTarget(const Surface& s, bool depth) {
    if (!s.header) return nullptr;
    TargetSlot slot = FindTargetSlot(s, depth);
    UpdateTargetUsed(slot.data);
    if (slot.sized) return &slot.it->second;
    if (slot.it != targets_.end()) ReleaseTarget(slot.it);
    VTarget t;
    t.depth = depth;
    t.gw = slot.gw;
    t.gh = slot.gh;
    if (!CreateTargetResource(t, s)) return nullptr;
    g_perf.targets++;
    t.data = slot.data;
    t.bytes = GuestPitch(slot.gw) * slot.gh;
    if (depth) t.bytes = slot.gw * slot.gh * 4;
    t.epoch = DirtyEpoch(slot.data);
    t.sig = MemSig(slot.data, t.bytes);
    auto& ref = targets_[slot.key] = std::move(t);
    return &ref;
}

void VulkanBackend::UpdateTargetUsed(uint32_t data) {
    for (const UsedTarget& u : recent_used_) {
        if (u.data == data && u.frame == frame_) return;
    }
    target_used_[data] = frame_;
    recent_used_[recent_used_next_++ % recent_used_.size()] = {data, frame_};
}

bool VulkanBackend::BindTargets(const State& s) {
    VTarget* rt = EnsureTarget(s.rt, false);
    VTarget* ds = s.rs[kRsZEnable] || s.rs[kRsStencilEnable] ? EnsureTarget(s.zs, true) : nullptr;
    if (rt && ds && (rt->w != ds->w || rt->h != ds->h)) ds = nullptr;
    if (!rt && !ds) return false;
    if (rt) {
        if (LatchCpuWrite(*rt)) UploadTarget(*rt, s.rt);
        rt->drawn = true;
    }
    if (ds) ds->drawn = true;
    bound_rt_ = rt;
    bound_ds_ = ds;
    want_ = {rt ? rt->img.get() : nullptr, ds ? ds->img.get() : nullptr};
    return true;
}

constexpr uint64_t kDepthCopyKey = ~0ull;

VkPipeline VulkanBackend::EnsureDepthCopyPipeline() {
    auto& pso = blit_psos_[kDepthCopyKey];
    if (pso) return pso;
    uint64_t h;
    VkShaderModule vs = Compile(kDepthHlsl, "vs", "vs_5_0", h);
    VkShaderModule ps = Compile(kDepthHlsl, "psmain", "ps_5_0", h);
    pso = FullScreenPipeline(vs, ps, VK_FORMAT_UNDEFINED, true, nullptr);
    return pso;
}

void VulkanBackend::DrawDepthCopy(VTarget& src, VTarget& dst, VkPipeline pso) {
    if (cap_) fprintf(cap_, "%04u depth copy %08X to %08X\n", cap_i_++, src.data, dst.data);
    Transition(*src.img, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    want_ = {nullptr, dst.img.get()};
    SetFullViewport(dst.w, dst.h);
    Alloc cb = Upload(256, 256);
    Image* views[4] = {src.img.get(), nullptr, nullptr, nullptr};
    DrawFullScreen(pso, cb, views);
    dst.drawn = true;
    bound_rt_ = bound_ds_ = nullptr;
}

bool VulkanBackend::DepthCopy(const State& s) {
    TextureDesc d;
    if (!s.rt.header || !s.textures[0] || !Describe(s.textures[0], d) || Format(d.format).depth || d.cube ||
        d.depth != 1) {
        return false;
    }
    uint32_t gw;
    uint32_t gh;
    SurfaceSize(s.rt, gw, gh);
    auto di = targets_.find(SizedTargetKey(mem::VirtualFromPhysical(s.rt.data), true, gw, gh));
    auto si = targets_.find(SizedTargetKey(d.data, true, d.width, d.height));
    if (di == targets_.end() || si == targets_.end() || di == si) return false;
    VkPipeline pso = EnsureDepthCopyPipeline();
    if (!pso) return false;
    DrawDepthCopy(si->second, di->second, pso);
    return true;
}

std::vector<uint8_t> LatchCpuChanges(VTarget& t) {
    std::vector<uint8_t> px(t.read.size(), 0);
    uint32_t guest_pitch = GuestPitch(t.gw);
    const uint8_t* guest = static_cast<const uint8_t*>(kernel::HostPtr(t.data));
    for (uint32_t y = 0; y < t.gh; y++) {
        for (uint32_t x = 0; x < t.gw; x++) {
            const uint8_t* c = guest + size_t(y) * guest_pitch + x * 4;
            size_t i = (size_t(y) * t.gw + x) * 4;
            if (!std::memcmp(c, &t.read[i], 4)) continue;
            std::memcpy(&t.read[i], c, 4);
            std::memcpy(&px[i], c, 3);
            px[i + 3] = 255;
        }
    }
    return px;
}

// BGRA pixels into a new w x h texture, left readable by shaders
ImageRef VulkanBackend::CreatePixelTexture(const std::vector<uint8_t>& px, uint32_t pitch, uint32_t w, uint32_t h) {
    ImageRef tex = MakeImage(kBgra, w, h, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    if (!tex) return nullptr;
    UploadTextureLevel(*tex, 0, 0, px, pitch);
    Transition(*tex, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    return tex;
}

void VulkanBackend::UploadTarget(VTarget& t, const Surface& s) {
    g_perf.rtups++;
    TextureDesc d;
    if (!Describe(s.header, d) || Format(d.format).depth) return;
    std::vector<uint8_t> px;
    uint32_t pitch;
    bool over = !t.drawn && d.width == t.gw && d.height == t.gh && t.read.size() == size_t(t.gw) * t.gh * 4;
    if (over) {
        LOG_FIRST(4, "vulkan: CPU pixels drawn over target %08X", t.data);
        pitch = t.gw * 4;
        px = LatchCpuChanges(t);
    } else {
        DecodeLevel(d, 0, 0, static_cast<const uint8_t*>(kernel::HostPtr(d.data)), nullptr, px, pitch);
    }
    ImageRef tex = CreatePixelTexture(px, pitch, d.width, d.height);
    if (!tex) return;
    Attach keep = want_;
    VTarget* keep_rt = bound_rt_;
    VTarget* keep_ds = bound_ds_;
    SetBlitTarget(*t.img);
    float uv[4] = {0, 0, 1, 1};
    Blit(*tex, uv, {.alpha = true, .rtv = t.fmt, .over = over});
    CurrentFrame().garbage.push_back(tex);
    want_ = keep;
    bound_rt_ = keep_rt;
    bound_ds_ = keep_ds;
    t.epoch = DirtyEpoch(t.data);
    t.sig = MemSig(t.data, t.bytes);
}

// rects of an image cleared to v, inside a rendering of it alone when none has it open
void VulkanBackend::ClearImage(Image& i, const std::vector<VkRect2D>& rects, const VkClearValue& v) {
    bool open = rendering_ && (active_.color == &i || active_.depth == &i);
    Attach keep = want_;
    if (!open) {
        want_ = i.depth ? Attach{nullptr, &i} : Attach{&i, nullptr};
        EnsureRendering();
    }
    VkClearAttachment ca{};
    ca.aspectMask = Aspects(i);
    ca.colorAttachment = 0;
    ca.clearValue = v;
    std::vector<VkClearRect> rs;
    for (const VkRect2D& r : rects)
        if (r.extent.width && r.extent.height) rs.push_back({r, 0, 1});
    if (!rs.empty()) vkCmdClearAttachments(cb_, 1, &ca, uint32_t(rs.size()), rs.data());
    want_ = keep;
}

VkRect2D ScaledRect(const VTarget& t, int x0, int y0, int x1, int y1) {
    float sc = float(t.w) / t.gw;
    int32_t l = std::clamp<int32_t>(int32_t(x0 * sc), 0, int32_t(t.w));
    int32_t tp = std::clamp<int32_t>(int32_t(y0 * sc), 0, int32_t(t.h));
    int32_t r = std::clamp<int32_t>(int32_t(x1 * sc), 0, int32_t(t.w));
    int32_t b = std::clamp<int32_t>(int32_t(y1 * sc), 0, int32_t(t.h));
    return {{l, tp}, {uint32_t(std::max(r - l, 0)), uint32_t(std::max(b - tp, 0))}};
}

void VulkanBackend::ClearTarget(VTarget& t, const State& s, const ClearArgs& c, std::vector<VkRect2D>& rs) {
    rs.clear();
    if (c.count && c.rects) {
        for (uint32_t i = 0; i < c.count; i++) {
            int32_t r[4];
            std::memcpy(r, c.rects + size_t(i) * sizeof(r), sizeof(r));
            rs.push_back(ScaledRect(t, r[0], r[1], r[2], r[3]));
        }
    } else {
        const auto& v = s.viewport;
        rs.push_back(ScaledRect(t, int(v.x), int(v.y), int(v.x + v.w), int(v.y + v.h)));
    }
    if (t.depth) {
        // depth and stencil apart: a clear of one keeps the other
        float reversed = 1 - std::clamp(c.z, 0.0f, 1.0f);
        VkClearValue v{};
        v.depthStencil = {reversed, c.stencil & 0xFF};
        bool open = rendering_ && active_.depth == t.img.get();
        Attach keep = want_;
        if (!open) {
            want_ = {nullptr, t.img.get()};
            EnsureRendering();
        }
        VkClearAttachment ca{};
        ca.aspectMask = ((c.flags & kClearZ) ? VK_IMAGE_ASPECT_DEPTH_BIT : 0) |
                        ((c.flags & kClearStencil) ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
        ca.clearValue = v;
        std::vector<VkClearRect> cr;
        for (const VkRect2D& r : rs)
            if (r.extent.width && r.extent.height) cr.push_back({r, 0, 1});
        if (ca.aspectMask && !cr.empty()) vkCmdClearAttachments(cb_, 1, &ca, uint32_t(cr.size()), cr.data());
        want_ = keep;
    } else {
        if (LatchCpuWrite(t)) UploadTarget(t, s.rt);
        float col[4];
        Color(c.color, col);
        uint8_t mask = uint8_t((c.flags & kClearTarget) >> 4);
        if (mask == 0xF) {
            VkClearValue v{};
            std::memcpy(v.color.float32, col, sizeof(col));
            ClearImage(*t.img, rs, v);
        } else {
            ClearChannels(t, col, mask, rs);
        }
    }
    t.drawn = true;
}

VkPipeline VulkanBackend::EnsureClearPipeline(VkFormat rtv, uint8_t mask) {
    uint64_t seed = uint64_t(rtv) << 8 | mask;
    auto& pso = blit_psos_[Hash(kClearHlsl, std::strlen(kClearHlsl), seed)];
    if (pso) return pso;
    uint64_t h;
    VkShaderModule vs = Compile(kClearHlsl, "vs", "vs_5_0", h);
    VkShaderModule ps = Compile(kClearHlsl, "psmain", "ps_5_0", h);
    // D3DCLEAR_TARGET's ARGB bits arrive in D3D12's RGBA write mask order, Vulkan's is the same
    pso = FullScreenPipeline(vs, ps, rtv, false, nullptr, mask);
    return pso;
}

void VulkanBackend::ClearChannels(VTarget& t, const float col[4], uint8_t mask, const std::vector<VkRect2D>& rs) {
    VkPipeline pso = EnsureClearPipeline(t.fmt, mask);
    if (!pso) return;
    Attach keep = want_;
    VTarget* keep_rt = bound_rt_;
    VTarget* keep_ds = bound_ds_;
    SetBlitTarget(*t.img);
    Alloc cb = Upload(256, 256);
    std::memcpy(cb.cpu, col, 4 * sizeof(float));
    Image* views[4] = {};
    for (const VkRect2D& r : rs) {
        if (!r.extent.width || !r.extent.height) continue;
        vkCmdSetScissor(cb_, 0, 1, &r);
        binds_ = {};
        DrawFullScreen(pso, cb, views);
    }
    want_ = keep;
    bound_rt_ = keep_rt;
    bound_ds_ = keep_ds;
    binds_ = {};
}

void VulkanBackend::Clear(const State& s, uint32_t count, const uint8_t* rects, uint32_t flags, uint32_t color, float z,
                          uint32_t stencil) {
    PerfScope perf_clear{g_perf.clear};
    if (cap_) {
        CapStep(s.rt);
        fprintf(cap_, "%04u clear rt %08X ds %08X flags %X color %08X z %.3f\n", cap_i_++, s.rt.data, s.zs.data, flags,
                color, z);
    }
    VTarget* rt = (flags & kClearTarget) ? EnsureTarget(s.rt, false) : nullptr;
    VTarget* ds = (flags & (kClearZ | kClearStencil)) ? EnsureTarget(s.zs, true) : nullptr;
    std::vector<VkRect2D> rs;
    ClearArgs c{.count = count, .rects = rects, .flags = flags, .color = color, .z = z, .stencil = stencil};
    for (VTarget* t : {rt, ds}) {
        if (t) ClearTarget(*t, s, c, rs);
    }
}

// Shaders and pipelines

std::string SpriteHlsl() {
    std::string s = PointHlsl();
    s.erase(s.find("[maxvertexcount"));
    return s + R"([maxvertexcount(4)]
void main(point VOut v[1], inout TriangleStream<VOut> strip) {
    float2 half_size = v[0].tc3.x * v[0].pos.w / surf.xy;
    for (int i = 0; i < 4; i++) {
        VOut o = v[0];
        float2 corner = float2(i & 1, i >> 1);
        o.pos.x += (corner.x * 2 - 1) * half_size.x;
        o.pos.y -= (corner.y * 2 - 1) * half_size.y;
        o.tc0 = float4(corner, 0, 1);
        o.tc3 = o.tc0;
        strip.Append(o);
    }
}
)";
}

constexpr uint32_t kFillWireframe = 0x1B01;
constexpr uint32_t kCullCw = 0x900;

// a D3DCMP test on reversed depth: less and greater trade places
VkCompareOp ReversedCmp(uint32_t v) {
    static const uint8_t mirror[8] = {0, 4, 2, 6, 1, 5, 3, 7};
    return VkCompareOp(mirror[Cmp(v)]);
}

VkPipeline VulkanBackend::MakePipeline(const PsoKey& k, VkShaderModule vs, VkShaderModule ps, VkShaderModule gs) {
    VkPipelineShaderStageCreateInfo st[3]{};
    uint32_t nst = 0;
    st[nst++] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vs, "main"};
    if (gs) st[nst++] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_GEOMETRY_BIT, gs, "main"};
    st[nst++] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, ps, "main"};
    // vertex inputs: locations in register order among the registers present; strides set per draw
    VkVertexInputAttributeDescription attrs[16];
    VkVertexInputBindingDescription bindings[16];
    uint32_t na = 0, nb = 0;
    uint32_t used_buffers = 0;
    for (uint32_t r = 0; r < 16; r++) {
        if (!((k.regs >> r) & 1)) continue;
        uint32_t e = k.elements[r];
        uint32_t buffer = (e >> 8) & 0xFF;
        attrs[na] = {na, buffer, VertexFormat(e & 0xFF), e >> 16};
        na++;
        used_buffers |= 1u << buffer;
    }
    for (uint32_t b = 0; b < 16; b++)
        if (used_buffers >> b & 1) bindings[nb++] = {b, 0, VK_VERTEX_INPUT_RATE_VERTEX};
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vi.vertexBindingDescriptionCount = nb;
    vi.pVertexBindingDescriptions = bindings;
    vi.vertexAttributeDescriptionCount = na;
    vi.pVertexAttributeDescriptions = attrs;
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = k.topo == uint32_t(Topology::Points)  ? VK_PRIMITIVE_TOPOLOGY_POINT_LIST
                  : k.topo == uint32_t(Topology::Lines) ? VK_PRIMITIVE_TOPOLOGY_LINE_LIST
                                                        : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = k.fill == kFillWireframe && !gs ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL;
    rs.cullMode = k.cull && !gs ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE;
    // D3D's front faces are clockwise on screen; the negative viewport height keeps the winding as D3D sees it
    rs.frontFace = k.ccw && !gs ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE;
    rs.depthBiasEnable = k.zslope != 0 || k.zbias != 0;
    rs.depthBiasConstantFactor = float(k.zbias);
    rs.depthBiasSlopeFactor = k.zslope;
    rs.lineWidth = 1;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    ds.depthTestEnable = k.zen;
    ds.depthWriteEnable = k.zwrite;
    ds.depthCompareOp = ReversedCmp(k.zfunc);
    ds.stencilTestEnable = k.sten;
    VkStencilOpState so{StencilOp(k.sfail), StencilOp(k.spass), StencilOp(k.szfail), Cmp(k.sfunc), k.smask, k.swmask, 0};
    ds.front = ds.back = so;
    VkPipelineColorBlendAttachmentState b{};
    b.blendEnable = k.blend;
    b.srcColorBlendFactor = Blend(k.src, false);
    b.dstColorBlendFactor = Blend(k.dst, false);
    b.colorBlendOp = BlendOp(k.op);
    b.srcAlphaBlendFactor = Blend(k.src, true);
    b.dstAlphaBlendFactor = Blend(k.dst, true);
    b.alphaBlendOp = BlendOp(k.op);
    b.colorWriteMask = k.wmask;
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = k.has_rt ? 1 : 0;
    cb.pAttachments = &b;
    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_BLEND_CONSTANTS,
                            VK_DYNAMIC_STATE_STENCIL_REFERENCE, VK_DYNAMIC_STATE_VERTEX_INPUT_BINDING_STRIDE,
                            VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY};
    VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dy.dynamicStateCount = uint32_t(std::size(dyn));
    dy.pDynamicStates = dyn;
    VkFormat color = VkFormat(k.rtfmt);
    VkPipelineRenderingCreateInfo ri{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    ri.colorAttachmentCount = k.has_rt ? 1 : 0;
    ri.pColorAttachmentFormats = &color;
    ri.depthAttachmentFormat = k.has_ds ? kDepthFormat : VK_FORMAT_UNDEFINED;
    ri.stencilAttachmentFormat = k.has_ds ? kDepthFormat : VK_FORMAT_UNDEFINED;
    VkGraphicsPipelineCreateInfo pi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pi.pNext = &ri;
    pi.stageCount = nst;
    pi.pStages = st;
    pi.pVertexInputState = &vi;
    pi.pInputAssemblyState = &ia;
    pi.pViewportState = &vp;
    pi.pRasterizationState = &rs;
    pi.pMultisampleState = &ms;
    pi.pDepthStencilState = &ds;
    pi.pColorBlendState = &cb;
    pi.pDynamicState = &dy;
    pi.layout = layout_;
    VkPipeline p = VK_NULL_HANDLE;
    if (vkCreateGraphicsPipelines(dev_, pcache_, 1, &pi, nullptr, &p) != VK_SUCCESS) return VK_NULL_HANDLE;
    return p;
}

VkPipeline VulkanBackend::EnsurePipeline(const PsoKey& k, VkShaderModule vs, VkShaderModule ps) {
    uint64_t h = Hash(&k, sizeof(k));
    auto it = psos_.find(h);
    if (it != psos_.end()) return it->second;
    {
        std::lock_guard lock(pre_mutex_);
        auto pre = pre_psos_.find(h);
        if (pre != pre_psos_.end()) {
            VkPipeline p = pre->second;
            psos_[h] = p;
            pre_psos_.erase(pre);
            return p;
        }
    }
    VkShaderModule gs = VK_NULL_HANDLE;
    if (k.topo == uint32_t(Topology::Points)) {
        uint64_t hash;
        gs = Compile(k.sprite ? SpriteHlsl() : PointHlsl(), "main", "gs_5_0", hash);
    }
    g_perf.psos++;
    VkPipeline p = MakePipeline(k, vs, ps, gs);
    if (!p) LOGE("vulkan: pipeline creation failed");
    else if (k.topo != uint32_t(Topology::Points)) ListPipeline(h, k);
    psos_[h] = p;
    return p;
}

void VulkanBackend::ListPipeline(uint64_t h, const PsoKey& k) {
    if (!pso_file_ || !pso_listed_.insert(h).second) return;
    fwrite(&k, sizeof(k), 1, pso_file_);
    fflush(pso_file_);
}

// cache/pipelines-vk.bin, read and made ahead on worker threads as d3d12.cpp's pipelines.bin
void VulkanBackend::StartPipelineWorkers() {
    std::error_code ec;
    std::filesystem::path dir = app::DataDir() / "cache";
    std::filesystem::create_directories(dir, ec);
    std::filesystem::path path =
        dir / (app::Instance() > 1 ? "pipelines-vk" + std::to_string(app::Instance()) + ".bin" : std::string("pipelines-vk.bin"));
    auto keys = std::make_shared<std::vector<PsoKey>>();
    if (FILE* f = OpenFile(path, "rb")) {
        uint32_t head[3] = {};
        if (fread(head, 4, 3, f) == 3 && head[0] == kPipelineCacheMagic && head[1] == kPipelineCacheVersion &&
            head[2] == sizeof(PsoKey)) {
            PsoKey k;
            while (fread(&k, sizeof(k), 1, f) == 1) {
                if (pso_listed_.insert(Hash(&k, sizeof(k))).second) keys->push_back(k);
            }
        }
        fclose(f);
    }
    pso_file_ = OpenFile(path, "wb");
    if (pso_file_) {
        uint32_t head[3] = {kPipelineCacheMagic, kPipelineCacheVersion, uint32_t(sizeof(PsoKey))};
        fwrite(head, 4, 3, pso_file_);
        if (!keys->empty()) fwrite(keys->data(), sizeof(PsoKey), keys->size(), pso_file_);
        fflush(pso_file_);
    }
    if (keys->empty() || std::getenv("XBE_NO_PSO_AHEAD")) return;
    auto next = std::make_shared<std::atomic<size_t>>(0);
    auto made = std::make_shared<std::atomic<size_t>>(0);
    auto left = std::make_shared<std::atomic<uint32_t>>(0);
    auto start = PerfClock::now();
    uint32_t threads = std::clamp(std::thread::hardware_concurrency() / 4, 1u, 4u);
    left->store(threads);
    for (uint32_t i = 0; i < threads; i++) {
        std::thread([this, keys, next, made, left, start, threads] {
            platform::NameThread("pipelines");
            for (size_t j; (j = next->fetch_add(1)) < keys->size();) {
                const PsoKey& k = (*keys)[j];
                if (k.topo == uint32_t(Topology::Points)) continue;
                const std::vector<uint32_t>* vs = g_shader_disk.Find(k.vs);
                const std::vector<uint32_t>* ps = g_shader_disk.Find(k.ps);
                if (!vs || !ps) continue;
                VkShaderModule vm = Module(*vs), pm = Module(*ps);
                VkPipeline p = vm && pm ? MakePipeline(k, vm, pm, VK_NULL_HANDLE) : VK_NULL_HANDLE;
                if (vm) vkDestroyShaderModule(dev_, vm, nullptr);
                if (pm) vkDestroyShaderModule(dev_, pm, nullptr);
                if (!p) continue;
                std::lock_guard lock(pre_mutex_);
                pre_psos_.emplace(Hash(&k, sizeof(k)), p);
                made->fetch_add(1);
            }
            if (left->fetch_sub(1) == 1) {
                LOGI("vulkan: %zu of %zu listed pipelines made ahead in %.0f ms on %u threads", made->load(),
                     keys->size(), PerfMs(start), threads);
            }
        }).detach();
    }
}

// HUD placement, as d3d12.cpp's

void VulkanBackend::FillWideConstants(DrawConstants& c) const {
    c.wide[0] = c.wide[1] = 0;
    c.wide[2] = 1;
    c.wide[3] = 0;
    c.hud[0] = c.hud[1] = c.hud[2] = c.hud[3] = 0;
    if (!HudPlacing() || !bound_rt_ || bound_rt_->gw != guest_w_ || bound_rt_->gh != guest_h_) return;
    c.wide[0] = 1.0f / wide_;
    c.wide[1] = 1;
    c.wide[2] = draw_side_;
    c.wide[3] = draw_sided_ ? 1.0f : 0.0f;
    if (!app::Get().hud_visible && draw_sided_ && draw_side_ != 2 && (draw_side_ != 0 || draw_vside_ != 0)) {
        c.hud[3] = 2;
        return;
    }
    if (draw_sided_ && (draw_dx_ > 0 || draw_dy_ > 0)) {
        c.hud[0] = draw_vside_;
        c.hud[1] = 2 * draw_dx_ / guest_w_ / wide_;
        c.hud[2] = 2 * draw_dy_ / guest_h_;
        c.hud[3] = 1;
    }
}

bool VulkanBackend::HudPlacing() const {
    const app::Settings& s = app::Get();
    return wide_ != 1.0f || s.hud_edge > 0 || !s.hud_visible;
}

void VulkanBackend::ChooseDrawSide(bool writes_depth) {
    draw_sided_ = false;
    draw_side_ = 0;
    draw_vside_ = 0;
    draw_dx_ = draw_dy_ = 0;
    float lo = draw_x_[0], hi = draw_x_[1], top = draw_y_[0], bottom = draw_y_[1];
    if (lo > hi) return;
    float w = float(guest_w_), h = float(app::kGame.view_h ? app::kGame.view_h : guest_h_);
    constexpr float kUnitSlack = 0.05f;
    if (lo >= -kUnitSlack && hi <= 1 + kUnitSlack) {
        lo *= w;
        hi *= w;
        top *= h;
        bottom *= h;
    }
    constexpr float kEdge = 1.0f;
    if (lo <= kEdge && hi >= w - kEdge) {
        draw_side_ = 2;
        draw_sided_ = true;
        return;
    }
    if (lo < -kEdge || hi > w + kEdge) return;
    draw_sided_ = true;
    constexpr float kNearEdge = 0.2f, kHalfPlus = 0.6f, kSlack = 2.0f;
    if (lo < w * kNearEdge && hi < w * kHalfPlus) draw_side_ = -1;
    else if (hi > w * (1 - kNearEdge) && lo > w * (1 - kHalfPlus)) draw_side_ = 1;
    if (top < h * kNearEdge && bottom < h * kHalfPlus) draw_vside_ = -1;
    else if (bottom > h * (1 - kNearEdge) && top > h * (1 - kHalfPlus)) draw_vside_ = 1;
    constexpr float kMarginX = 32, kMarginY = 18, kGap = 6;
    float edge = app::Get().hud_edge / 100.0f;
    auto move = [&](float margin, float distance) { return std::max(0.0f, std::min(margin * edge, distance - kGap)); };
    if (draw_side_ == -1) draw_dx_ = move(kMarginX, lo);
    if (draw_side_ == 1) draw_dx_ = move(kMarginX, w - hi);
    if (draw_vside_ == -1) draw_dy_ = move(kMarginY, top);
    if (draw_vside_ == 1) draw_dy_ = move(kMarginY, h - bottom);
    bool inside = false;
    for (const auto* list : {&sided_, &prev_sided_}) {
        for (const SidedPiece& p : *list)
            if (lo >= p.lo - kSlack && hi <= p.hi + kSlack && top >= p.top - kSlack && bottom <= p.bottom + kSlack) {
                draw_side_ = p.side;
                draw_vside_ = p.vside;
                draw_dx_ = p.dx;
                draw_dy_ = p.dy;
                inside = true;
                break;
            }
        if (inside) break;
    }
    constexpr float kBeside = 16.0f;
    for (const auto* list : {&sided_, &prev_sided_}) {
        if (inside || draw_side_ != 0) break;
        for (const SidedPiece& p : *list) {
            float overlap = std::min(bottom, p.bottom) - std::max(top, p.top);
            float shorter = std::min(bottom - top, p.bottom - p.top);
            float gap = std::max(p.lo - hi, lo - p.hi);
            if (p.side == 0 && shorter > 0 && overlap >= shorter * 0.5f && gap <= kBeside) {
                draw_vside_ = p.vside;
                draw_dy_ = p.dy;
                inside = true;
                break;
            }
        }
    }
    constexpr size_t kMaxPieces = 256;
    constexpr float kMaxCarrier = 0.55f, kMaxCarrierH = 0.25f;
    float hh = std::fabs(bottom - top);
    if ((draw_side_ == 0 && draw_vside_ == 0) || writes_depth || hi - lo > w * kMaxCarrier || hh > h * kMaxCarrierH)
        return;
    constexpr float kTouch = 4.0f;
    SidedPiece piece{lo, hi, top, bottom, draw_side_, draw_vside_, draw_dx_, draw_dy_};
    for (auto it = sided_.begin(); it != sided_.end();) {
        bool touches = it->side == piece.side && it->vside == piece.vside && it->lo <= piece.hi + kTouch &&
                       piece.lo <= it->hi + kTouch && it->top <= piece.bottom + kTouch && piece.top <= it->bottom + kTouch;
        float merged = std::max(it->hi, piece.hi) - std::min(it->lo, piece.lo);
        float merged_h = std::max(it->bottom, piece.bottom) - std::min(it->top, piece.top);
        if (touches && merged <= w * kMaxCarrier && merged_h <= h * kMaxCarrierH) {
            piece = {std::min(it->lo, piece.lo), std::max(it->hi, piece.hi), std::min(it->top, piece.top),
                     std::max(it->bottom, piece.bottom), piece.side, piece.vside, std::min(it->dx, piece.dx),
                     std::min(it->dy, piece.dy)};
            it = sided_.erase(it);
        } else {
            ++it;
        }
    }
    if (sided_.size() < kMaxPieces) sided_.push_back(piece);
}

void VulkanBackend::MeasureDrawX(const Assembled& a, bool writes_depth) {
    draw_x_[0] = draw_y_[0] = 1;
    draw_x_[1] = draw_y_[1] = 0;
    draw_sided_ = false;
    draw_side_ = 0;
    constexpr uint32_t kMaxMeasured = 4096;
    if (!(a.regs & 1) || a.vertex_count > kMaxMeasured) return;
    const VertexElement& e = a.elements[0];
    float lo = 1e30f, hi = -1e30f, top = 1e30f, bottom = -1e30f;
    auto take = [&](float x, float y) {
        lo = std::min(lo, x);
        hi = std::max(hi, x);
        top = std::min(top, y);
        bottom = std::max(bottom, y);
    };
    if (!a.vertices.empty()) {
        uint32_t n = 0;
        for (int r = 0; r < 16; r++) n += (a.regs >> r) & 1;
        for (size_t v = 0; v * n * 4 < a.vertices.size(); v++) take(a.vertices[v * n * 4], a.vertices[v * n * 4 + 1]);
    } else {
        bool fl = e.format == DXGI_FORMAT_R32G32_FLOAT || e.format == DXGI_FORMAT_R32G32B32_FLOAT ||
                  e.format == DXGI_FORMAT_R32G32B32A32_FLOAT;
        const VertexBuffer& b = a.buffers[e.buffer];
        if (!fl || !b.src || !b.stride) return;
        for (uint32_t off = e.offset; off + 8 <= b.bytes; off += b.stride) {
            float xy[2];
            std::memcpy(xy, b.src + off, 8);
            take(xy[0], xy[1]);
        }
    }
    if (lo <= hi && std::isfinite(lo) && std::isfinite(hi) && std::isfinite(top) && std::isfinite(bottom)) {
        draw_x_[0] = lo;
        draw_x_[1] = hi;
        draw_y_[0] = top;
        draw_y_[1] = bottom;
    }
    if (HudPlacing()) ChooseDrawSide(writes_depth);
}

bool VulkanBackend::DrawIs2D(const State& s, const Assembled& a, bool writes_depth) const {
    if (!bound_rt_ || bound_rt_->gw != guest_w_ || bound_rt_->gh != guest_h_ || writes_depth) return false;
    if (s.rs[kRsZEnable] == 0 || Cmp(SimpleState(s, kMZFunc)) == VK_COMPARE_OP_ALWAYS) return true;
    if (draw_x_[0] > draw_x_[1]) return false;
    constexpr float kUnitSlack = 0.05f;
    bool unit = draw_x_[0] >= -kUnitSlack && draw_x_[1] <= 1 + kUnitSlack && draw_y_[0] >= -kUnitSlack &&
                draw_y_[1] <= 1 + kUnitSlack;
    return unit || a.pretransformed;
}

float VulkanBackend::TargetScale() const {
    const VTarget* t = bound_rt_ ? bound_rt_ : bound_ds_;
    if (!t || !t->gw) return 1;
    return float(t->w) / float(t->gw);
}

float VulkanBackend::RenderTargetShift(const State& s, const DrawShaders& sh) const {
    bool offset = !sh.prog || ReadsViewportOffset(s.vs->program);
    return offset ? -0.5f * TargetScale() : 0.0f;
}

// Textures

TexKind TextureKind(const TextureDesc& d) {
    if (d.cube) return kTexCube;
    if (d.depth > 1) return kTex3D;
    return kTex2D;
}

constexpr float kUpscaledMin = 1.01f;

Image* VulkanBackend::BindTargetTexture(const State& s, int stage, const TextureDesc& d, VTarget& t, bool box,
                                        DrawTextures& stages) {
    TexInfo& info = stages.info[stage];
    if (&t == draw_ds_) return nullptr;
    bool feedback = &t == draw_rt_;
    stages.rt[stage] = true;
    info.flags |= kTexTarget;
    if (!t.depth && LatchCpuWrite(t)) {
        Surface su;
        su.header = s.textures[size_t(stage)];
        UploadTarget(t, su);
    }
    info.kind = kTex2D;
    VTarget* rt = FindTarget(s.rt, false);
    float k = float(t.w) / (t.gw * stages.k[stage]);
    if (!t.depth && box && k > kUpscaledMin && rt && rt != &t && (t.gw > rt->gw || t.gh > rt->gh)) {
        stages.wide[stage] = k;
        info.flags |= kTexWide;
    }
    if (d.linear) {
        stages.tsize[stage][0] = t.gw;
        stages.tsize[stage][1] = t.gh;
    }
    stages.type[stage] = VK_IMAGE_VIEW_TYPE_2D;
    if (feedback) return Feedback(t);
    Transition(*t.img, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    if (t.depth) info.shadow = ShadowDepthKind(d.format);
    return t.img.get();
}

bool SameTexture(const Tex& tx, const TextureDesc& d) {
    return tx.img && tx.d.format == d.format && tx.d.width == d.width && tx.d.height == d.height &&
           tx.d.depth == d.depth && tx.d.levels == d.levels && tx.d.cube == d.cube && tx.d.pitch == d.pitch;
}

bool VulkanBackend::CreateTexture(Tex& tx, const TextureDesc& d, const FormatInfo& fi, uint64_t key) {
    g_perf.newtex++;
    if (tx.img) CurrentFrame().garbage.push_back(tx.img);
    tx = Tex{};
    tx.d = d;
    tx.fmt = HostFormat(fi.host);
    uint32_t w = d.width;
    uint32_t h = d.height;
    tx.levels = d.levels;
    if (fi.compressed && ((w & 3) || (h & 3))) {
        w = (w + 3) & ~3u;
        h = (h + 3) & ~3u;
        tx.levels = 1;
    }
    tx.img = MakeImage(tx.fmt, w, h, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, false, tx.levels,
                       d.depth > 1 ? d.depth : 1, d.cube);
    if (!tx.img) {
        textures_.erase(key);
        return false;
    }
    return true;
}

// one level of one face from rows of pitch bytes: the image's rows at that level, short ones padded with zeros
void VulkanBackend::UploadTextureLevel(Image& img, uint32_t f, uint32_t l, const std::vector<uint8_t>& px,
                                       uint32_t pitch) {
    uint32_t w = std::max(img.w >> l, 1u), h = std::max(img.h >> l, 1u), dd = std::max(img.d >> l, 1u);
    bool bc = IsBc(img.fmt);
    uint32_t row = bc ? (w + 3) / 4 * BlockBytes(img.fmt) : w * TexelBytes(img.fmt);
    uint32_t rows = bc ? (h + 3) / 4 : h;
    uint64_t bytes = uint64_t(row) * rows * dd;
    Alloc a = Upload(bytes, 16);
    size_t src_rows = pitch ? px.size() / pitch / dd : 0;
    for (uint32_t z = 0; z < dd; z++) {
        for (uint32_t y = 0; y < rows; y++) {
            uint8_t* dst = a.cpu + (size_t(z) * rows + y) * row;
            if (y < src_rows) std::memcpy(dst, &px[(size_t(z) * src_rows + y) * pitch], std::min<size_t>(pitch, row));
            else std::memset(dst, 0, row);
            if (pitch < row && y < src_rows) std::memset(dst + pitch, 0, row - pitch);
        }
    }
    Transition(img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkBufferImageCopy c{};
    c.bufferOffset = a.off;
    c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, l, f, 1};
    c.imageExtent = {w, h, dd};
    vkCmdCopyBufferToImage(cb_, a.buf, img.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
}

void VulkanBackend::UploadTexture(Tex& tx, const TextureDesc& d, const uint8_t* p, const uint32_t* colours) {
    uint32_t faces = d.cube ? 6 : 1;
    std::vector<uint8_t> px;
    for (uint32_t f = 0; f < faces; f++) {
        for (uint32_t l = 0; l < tx.levels; l++) {
            uint32_t pitch;
            DecodeLevel(d, l, f, p, colours, px, pitch);
            UploadTextureLevel(*tx.img, f, l, px, pitch);
        }
    }
    Transition(*tx.img, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

struct StagePalette {
    uint32_t colours[256] = {};
    uint32_t va = 0;
    uint64_t hash = 0;
};

void ReadStagePalette(const State& s, int stage, const TextureDesc& d, StagePalette& palette) {
    constexpr uint32_t kFmtP8 = 0x0B;
    constexpr uint32_t kPaletteCommon = 0;
    constexpr uint32_t kPaletteData = 4;
    constexpr uint32_t kPaletteSizeShift = 30;
    if (d.format != kFmtP8 || !s.palettes[size_t(stage)]) return;
    uint32_t header = s.palettes[size_t(stage)];
    palette.va = mem::VirtualFromPhysical(kernel::R32(header + kPaletteData));
    uint32_t size_code = kernel::R32(header + kPaletteCommon) >> kPaletteSizeShift;
    size_t bytes = (std::size(palette.colours) >> size_code) * sizeof(uint32_t);
    std::memcpy(palette.colours, kernel::HostPtr(palette.va), bytes);
    palette.hash = Hash(palette.colours, sizeof(palette.colours));
}

Image* VulkanBackend::BindGuestTexture(const State& s, int stage, const TextureDesc& d, const FormatInfo& fi,
                                       DrawTextures& stages) {
    TexInfo& info = stages.info[stage];
    StagePalette palette;
    ReadStagePalette(s, stage, d, palette);
    const uint64_t key = uint64_t(palette.va) << 32 | d.data;
    const uint32_t* colours = palette.va ? palette.colours : nullptr;
    Tex& tx = textures_[key];
    uint32_t total = TotalSize(d);
    bool same = SameTexture(tx, d);
    if (!same && !CreateTexture(tx, d, fi, key)) return nullptr;
    if (tx.frame != presents_ + 1 || !same || tx.palette != palette.hash) {
        tx.frame = presents_ + 1;
        tx.palette = palette.hash;
        const uint8_t* p = static_cast<const uint8_t*>(kernel::HostPtr(d.data));
        auto perf_t = PerfClock::now();
        uint64_t sig = MemSig(d.data, total) ^ DirtyEpoch(d.data) ^ palette.hash;
        g_perf.sig += PerfMs(perf_t);
        if (sig != tx.sig || !same) {
            PerfScope perf_up{g_perf.texup};
            g_perf.texups++;
            g_perf.texup_bytes += total;
            tx.sig = sig;
            UploadTexture(tx, d, p, colours);
            tx.pack_gen = 0;
        }
    }
    if (draw_ui_ && !tx.ui) {
        tx.ui = true;
        tx.pack_gen = 0;
    }
    if (tx.pack_gen != texturedumper::Generation()) LatchPackKey(tx, d, fi, colours);
    if (tx.pack_key) LatchPackImage(tx);
    const Tex& drawn = tx.swap ? *tx.swap : tx;
    stages.type[stage] = d.cube ? VK_IMAGE_VIEW_TYPE_CUBE : d.depth > 1 ? VK_IMAGE_VIEW_TYPE_3D : VK_IMAGE_VIEW_TYPE_2D;
    stages.levels[stage] = drawn.levels;
    if (fi.depth) info.shadow = ShadowDepthKind(d.format);
    Transition(*drawn.img, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    return drawn.img.get();
}

void VulkanBackend::LatchPackKey(Tex& tx, const TextureDesc& d, const FormatInfo& fi, const uint32_t* colours) {
    const uint8_t* p = static_cast<const uint8_t*>(kernel::HostPtr(d.data));
    tx.pack_gen = texturedumper::Generation();
    tx.swap = nullptr;
    uint64_t key = texturedumper::Key(d, fi, p, colours);
    texturedumper::Dump(key, d, p, colours);
    bool edited = texturedumper::Edit(key, d, p, colours, tx.ui);
    tx.pack_key = edited || texturedumper::Replacing() ? key : 0;
}

void VulkanBackend::LatchPackImage(Tex& tx) {
    auto it = pack_textures_.find(tx.pack_key);
    if (it == pack_textures_.end()) {
        texturedumper::Image img;
        texturedumper::Lookup got = texturedumper::Take(tx.pack_key, img);
        if (got == texturedumper::Lookup::Loading) return;
        if (got == texturedumper::Lookup::None) {
            tx.pack_key = 0;
            return;
        }
        it = pack_textures_.emplace(tx.pack_key, Tex{}).first;
        CreatePackTexture(it->second, img);
    }
    if (it->second.img) tx.swap = &it->second;
    tx.pack_key = 0;
}

void VulkanBackend::CreatePackTexture(Tex& r, const texturedumper::Image& img) {
    r.fmt = FromDxgi(img.format);
    r.levels = uint32_t(img.levels.size());
    r.img = MakeImage(r.fmt, img.width, img.height, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, false,
                      r.levels);
    if (!r.img) {
        LOGE("texture pack: a %ux%u image could not be made", img.width, img.height);
        return;
    }
    for (uint32_t l = 0; l < r.levels; l++) UploadTextureLevel(*r.img, 0, l, img.levels[l].px, img.levels[l].pitch);
    Transition(*r.img, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

void VulkanBackend::ReleasePackTextures() {
    for (auto& entry : pack_textures_) CurrentFrame().garbage.push_back(entry.second.img);
    pack_textures_.clear();
}

Image* VulkanBackend::BindTexture(const State& s, int stage, DrawTextures& stages) {
    TexInfo& info = stages.info[stage];
    info = {};
    stages.rt[stage] = false;
    stages.k[stage] = 1;
    stages.wide[stage] = 0;
    stages.tsize[stage][0] = stages.tsize[stage][1] = 0;
    stages.type[stage] = VK_IMAGE_VIEW_TYPE_2D;
    stages.levels[stage] = 1;
    TextureDesc d;
    if (!s.textures[size_t(stage)] || !Describe(s.textures[size_t(stage)], d)) return nullptr;
    const FormatInfo& fi = Format(d.format);
    info.kind = TextureKind(d);
    info.rect = d.linear;
    auto tit = targets_.find(SizedTargetKey(d.data, fi.depth, d.width, d.height));
    if (tit != targets_.end() && !d.cube && d.depth == 1 && !fi.compressed) {
        return BindTargetTexture(s, stage, d, tit->second, true, stages);
    }
    return BindGuestTexture(s, stage, d, fi, stages);
}

constexpr uint32_t kTexfLinear = 2;
constexpr uint32_t kTexfAnisotropic = 3;
constexpr uint32_t kTaddressWrap = 1;
constexpr uint32_t kTaddressClamp = 3;

VkSampler VulkanBackend::StageSampler(const State& s, int i, const DrawTextures& stages, bool clamp, bool fixed,
                                      uint32_t aniso) {
    VkSamplerCreateInfo ci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    const auto& t = s.tss[size_t(i)];
    uint32_t mag = t[kTssMagFilter];
    uint32_t min = t[kTssMinFilter];
    uint32_t mip = t[kTssMipFilter];
    auto lin = [](uint32_t f) { return f >= kTexfLinear; };
    ci.minFilter = lin(min) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    ci.magFilter = lin(mag) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    ci.mipmapMode = mip == kTexfLinear ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    bool anisotropic = min == kTexfAnisotropic || mag == kTexfAnisotropic;
    uint32_t an = anisotropic ? std::max<uint32_t>(t[kTssMaxAnisotropy], 1) : 1;
    if (aniso && lin(min) && mip && stages.levels[i] > 1 && !stages.info[i].shadow) an = aniso;
    if (an > 1) {
        // D3D12's anisotropic filter is linear in all three
        ci.minFilter = ci.magFilter = VK_FILTER_LINEAR;
        ci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        ci.anisotropyEnable = VK_TRUE;
        ci.maxAnisotropy = float(std::min<uint32_t>(an, 16));
    }
    if (fixed && ((t[kTssTexCoordIndex] >> 16 & 7) || (t[kTssTextureTransformFlags] & 7))) clamp = false;
    ci.addressModeU = Address(clamp && t[kTssAddressU] == kTaddressWrap ? kTaddressClamp : t[kTssAddressU]);
    ci.addressModeV = Address(clamp && t[kTssAddressV] == kTaddressWrap ? kTaddressClamp : t[kTssAddressV]);
    ci.addressModeW = Address(t[kTssAddressW]);
    ci.mipLodBias = std::clamp(Bits(t[kTssMipLodBias]), -16.0f, 15.99f);
    ci.minLod = float(t[kTssMaxMipLevel]);
    ci.maxLod = mip ? VK_LOD_CLAMP_NONE : ci.minLod;
    // the border is black or white in Vulkan's core: the nearer of the two to the title's colour
    float bc[4];
    Color(t[kTssBorderColor], bc);
    bool white = bc[0] + bc[1] + bc[2] > 1.5f;
    ci.borderColor = bc[3] < 0.5f ? VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK
                                  : white ? VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE : VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
    if (stages.info[i].shadow) {
        ci.minFilter = ci.magFilter = lin(min) || lin(mag) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
        ci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        ci.compareEnable = VK_TRUE;
        ci.compareOp = Cmp(s.rs[kRsShadowFunc]);
        ci.anisotropyEnable = VK_FALSE;
        ci.maxAnisotropy = 0;
    }
    return EnsureSampler(ci);
}

// Draws

constexpr uint32_t kQuadVertices = 6;

DrawShaders ClassifyShaders(const State& s, const Assembled& a, uint32_t count) {
    DrawShaders sh;
    sh.prog = (s.vs_handle & 1) && s.vs && !s.vs->program.empty();
    bool points = a.topology == Topology::Points;
    sh.quad = count <= kQuadVertices && !s.rs[kRsZEnable] && !points;
    sh.fixed_transform = !sh.prog && !a.pretransformed;
    sh.sprite = points && sh.prog && s.rs[kRsPointSpriteEnable];
    if (!sh.prog) sh.fk = MakeFixedKey(s);
    return sh;
}

void EnsureProgramHash(const State& s) {
    if (!s.vs->hash) s.vs->hash = Hash(s.vs->program.data(), s.vs->program.size() * 4) | 1;
}

constexpr uint64_t kDeclarationSalt = 0x10000;
constexpr uint64_t kPretransformedSalt = 0x5A5A;
constexpr uint64_t kQuadSalt = 0xC3C30000;
constexpr uint64_t kSpriteSalt = 0x3C3C0000;

uint64_t VertexKey(const State& s, const Assembled& a, const DrawShaders& sh) {
    uint64_t seed = sh.prog ? s.vs->hash ^ a.regs : Hash(&s.vs_handle, 4, a.regs | kDeclarationSalt);
    uint64_t vkey = Hash(&a.decode, 8, seed);
    if (!sh.prog && a.pretransformed) vkey ^= kPretransformedSalt;
    if (sh.quad) vkey ^= kQuadSalt;
    if (sh.sprite) vkey ^= kSpriteSalt;
    if (!sh.prog) vkey = Hash(&sh.fk, sizeof(sh.fk), vkey);
    return vkey;
}

std::string SpriteSizeHlsl(std::string s) {
    const char* tc3 = "    o.tc3 = oT3;\n";
    size_t at = s.find(tc3);
    if (at == std::string::npos) {
        LOGE("vulkan: a vertex shader no longer fits SpriteSizeHlsl");
        return s;
    }
    s.replace(at, std::strlen(tc3), "    o.tc3 = oPts;\n");
    return s;
}

std::string VertexHlsl(const State& s, const Assembled& a, const DrawShaders& sh) {
    if (sh.prog) {
        std::string src = VertexProgramHlsl(s.vs->program, a.regs, a.decode);
        return sh.sprite ? SpriteSizeHlsl(std::move(src)) : src;
    }
    return FixedVertexHlsl(a.regs, a.pretransformed, a.decode, sh.fk);
}

void VulkanBackend::EnsureVertexShader(const State& s, const Assembled& a, DrawShaders& sh) {
    auto vit = shader_src_.find(sh.vkey);
    if (vit != shader_src_.end()) {
        std::lock_guard lock(module_mutex_);
        sh.vs_code = modules_[vit->second];
        sh.vh = vit->second;
        return;
    }
    std::string src = ReversedDepthHlsl(VertexHlsl(s, a, sh));
    sh.vs_code = Compile(sh.quad ? QuadHlsl(std::move(src)) : src, "main", "vs_5_0", sh.vh);
    shader_src_[sh.vkey] = sh.vh;
}

bool VulkanBackend::BindStageTexturesAndTargets(const State& s, DrawTextures& stages) {
    draw_rt_ = bound_rt_;
    draw_ds_ = bound_ds_;
    Attach keep = want_;
    {
        PerfScope perf_bind{g_perf.bind};
        for (int i = 0; i < 4; i++) stages.res[i] = BindTexture(s, i, stages);
    }
    if (bound_rt_ != draw_rt_ || bound_ds_ != draw_ds_) return BindTargets(s);
    want_ = keep;
    return true;
}

constexpr uint64_t kPixelKeySalt = 0x9E3779B97F4A7C15ull;

void VulkanBackend::EnsurePixelShader(const PixelKey& pk, DrawShaders& sh) {
    auto pit = shader_src_.find(sh.pkey ^ kPixelKeySalt);
    if (pit != shader_src_.end()) {
        std::lock_guard lock(module_mutex_);
        sh.ps_code = modules_[pit->second];
        sh.ph = pit->second;
        return;
    }
    sh.ps_code = Compile(PixelHlsl(pk), "main", "ps_5_0", sh.ph);
    shader_src_[sh.pkey ^ kPixelKeySalt] = sh.ph;
}

void KeyBlend(const State& s, PsoKey& k) {
    k.blend = SimpleState(s, kMBlendEnable) != 0;
    if (k.blend) {
        k.src = SimpleState(s, kMSrcBlend);
        k.dst = SimpleState(s, kMDestBlend);
        k.op = SimpleState(s, kMBlendOp);
    }
    uint32_t cw = SimpleState(s, kMColorWriteEnable);
    k.wmask = ((cw >> 16) & 1) | ((cw >> 8) & 1) << 1 | (cw & 1) << 2 | ((cw >> 24) & 1) << 3;
}

constexpr uint32_t kZUseW = 2;

bool OffsetsPolygons(const State& s, Topology topology) {
    return topology == Topology::Triangles && SimpleState(s, kMSolidOffsetEnable);
}

float HostSlopeScale(const State& s, const VTarget* ds) {
    return Bits(SimpleState(s, kMSlopeScale)) * (ds ? float(ds->w) / ds->gw : 1.0f);
}

void VulkanBackend::KeyDepthStencil(const State& s, const Assembled& a, PsoKey& k) {
    k.zen = s.rs[kRsZEnable] != 0;
    if (k.zen) {
        k.zwrite = SimpleState(s, kMZWriteEnable) != 0;
        k.zfunc = SimpleState(s, kMZFunc);
    }
    k.sten = s.rs[kRsStencilEnable] != 0;
    if (k.sten) {
        k.sfunc = SimpleState(s, kMStencilFunc);
        k.smask = SimpleState(s, kMStencilMask) & 0xFF;
        k.swmask = SimpleState(s, kMStencilWriteMask) & 0xFF;
        k.sfail = s.rs[kRsStencilFail];
        k.szfail = SimpleState(s, kMStencilZFail);
        k.spass = SimpleState(s, kMStencilPass);
    }
    if (OffsetsPolygons(s, a.topology)) k.zslope = -HostSlopeScale(s, bound_ds_);
}

void KeyRasterizer(const State& s, const Assembled& a, PsoKey& k) {
    uint32_t cull = s.rs[kRsCullMode];
    k.cull = cull != 0 && a.topology == Topology::Triangles;
    k.ccw = cull == kCullCw;
    k.fill = s.rs[kRsFillMode];
}

void VulkanBackend::BuildPipelineKey(const State& s, const Assembled& a, uint64_t vh, uint64_t ph, PsoKey& k) {
    std::memset(&k, 0, sizeof(k));
    k.vs = vh;
    k.ps = ph;
    k.regs = a.regs;
    for (int r = 0; r < 16; r++) {
        if ((a.regs >> r) & 1) {
            const VertexElement& e = a.elements[size_t(r)];
            k.elements[r] = e.format | uint32_t(e.buffer) << 8 | uint32_t(e.offset) << 16;
        }
    }
    k.topo = uint32_t(a.topology);
    k.has_rt = bound_rt_ != nullptr;
    k.has_ds = bound_ds_ != nullptr;
    k.rtfmt = bound_rt_ ? uint32_t(bound_rt_->fmt) : 0;
    KeyBlend(s, k);
    if (k.has_ds) KeyDepthStencil(s, a, k);
    KeyRasterizer(s, a, k);
}

// Constants, as d3d12.cpp's (the shaders' alpha and shadow functions take D3D12's comparison order minus 1, which is
// Vulkan's)
constexpr float kAlphaTestAlways = 7.0f;
constexpr uint32_t kFogTableNone = 0;
constexpr float kFogTableLinear = 3.0f;

void FillFogConstants(const State& s, bool vertex_program, DrawConstants& c) {
    if (!s.rs[kRsFogEnable]) return;
    if (vertex_program && s.rs[kRsFogTableMode] == kFogTableNone) {
        c.fog[0] = 1.0f;
        c.fog[1] = 0.0f;
        c.fog[2] = 0.0f;
        c.fog[3] = kFogTableLinear;
        return;
    }
    c.fog[0] = Bits(s.rs[kRsFogStart]);
    c.fog[1] = Bits(s.rs[kRsFogEnd]);
    c.fog[2] = Bits(s.rs[kRsFogDensity]);
    c.fog[3] = float(s.rs[kRsFogTableMode]);
}

void FillSurfaceConstants(const State& s, GuestExtent size, DrawConstants& c) {
    c.surf[0] = float(size.w);
    c.surf[1] = float(size.h);
    c.surf[2] = 1.0f / s.zscale;
    Color(s.rs[kRsFogColor], c.fog_color);
    c.misc[0] = float(SimpleState(s, kMAlphaRef) & 0xFF);
    c.misc[1] = SimpleState(s, kMAlphaTestEnable) ? float(Cmp(SimpleState(s, kMAlphaFunc))) : kAlphaTestAlways;
    c.misc[2] = float(Cmp(s.rs[kRsShadowFunc]));
    c.misc[3] = s.rs[kRsSpecularEnable] ? 1.0f : 0.0f;
}

void FillCombinerConstants(const State& s, DrawConstants& c) {
    for (int i = 0; i < 8; i++) {
        Color(SimpleState(s, kMPsConstant0 + i * 4), c.pc[i * 2]);
        Color(SimpleState(s, kMPsConstant1 + i * 4), c.pc[i * 2 + 1]);
    }
    Color(SimpleState(s, kMPsFinalConstant0), c.pc[16]);
    Color(SimpleState(s, kMPsFinalConstant1), c.pc[17]);
    Color(s.rs[kRsTextureFactor], c.tfactor);
}

constexpr float kGlowBoxTexels = 3.0f;

void VulkanBackend::FillTexelConstants(int i, const TextureDesc& d, const DrawTextures& stages, DrawConstants& c) {
    c.texsize[i][0] = float(d.width);
    c.texsize[i][1] = float(d.height);
    if (!stages.rt[i]) return;
    if (stages.tsize[i][0] && stages.tsize[i][1]) {
        c.texsize[i][0] = float(stages.tsize[i][0]);
        c.texsize[i][1] = float(stages.tsize[i][1]);
    }
    float k = stages.k[i];
    if (stages.wide[i] > 0) {
        float n = std::ceil(kGlowBoxTexels * stages.wide[i] * 0.5f);
        c.spread[i][0] = kGlowBoxTexels / (n * float(d.width) * k);
        c.spread[i][1] = kGlowBoxTexels / (n * float(d.height) * k);
        c.spread[i][2] = n;
    }
}

void VulkanBackend::FillTextureConstants(const State& s, const DrawShaders& sh, const DrawTextures& stages,
                                         DrawConstants& c) {
    for (int i = 0; i < 4; i++) {
        TextureDesc d;
        if (s.textures[size_t(i)] && Describe(s.textures[size_t(i)], d)) FillTexelConstants(i, d, stages, c);
        if (stages.rt[i]) c.rtshift[i] = RenderTargetShift(s, sh);
        const auto& t = s.tss[size_t(i)];
        c.bump[i][0] = Bits(t[kTssBumpEnvMat00]);
        c.bump[i][1] = Bits(t[kTssBumpEnvMat01]);
        c.bump[i][2] = Bits(t[kTssBumpEnvMat10]);
        c.bump[i][3] = Bits(t[kTssBumpEnvMat11]);
        c.bumpl[i][0] = Bits(t[kTssBumpEnvLScale]);
        c.bumpl[i][1] = Bits(t[kTssBumpEnvLOffset]);
    }
}

void MatMul(const float* x, const float* y, float* o) {
    for (int r = 0; r < 4; r++)
        for (int col = 0; col < 4; col++)
            o[r * 4 + col] = x[r * 4] * y[col] + x[r * 4 + 1] * y[4 + col] + x[r * 4 + 2] * y[8 + col] + x[r * 4 + 3] * y[12 + col];
}

void Transpose(const float* m, float* o) {
    for (int r = 0; r < 4; r++)
        for (int col = 0; col < 4; col++) o[col * 4 + r] = m[r * 4 + col];
}

void InverseTranspose3x3(const float* wv, float* n) {
    float det = wv[0] * (wv[5] * wv[10] - wv[6] * wv[9]) - wv[1] * (wv[4] * wv[10] - wv[6] * wv[8]) +
                wv[2] * (wv[4] * wv[9] - wv[5] * wv[8]);
    float id = det != 0 ? 1.0f / det : 0.0f;
    for (int r = 0; r < 3; r++)
        for (int col = 0; col < 3; col++) {
            int r1 = (r + 1) % 3, r2 = (r + 2) % 3, c1 = (col + 1) % 3, c2 = (col + 2) % 3;
            n[r * 4 + col] = (wv[r1 * 4 + c1] * wv[r2 * 4 + c2] - wv[r1 * 4 + c2] * wv[r2 * 4 + c1]) * id;
        }
}

constexpr size_t kTsView = 2;
constexpr size_t kTsProjection = 3;
constexpr size_t kTsTexture0 = 16;
constexpr size_t kTsWorld = 256;

void FillTransformConstants(const State& s, GuestExtent size, DrawConstants& c) {
    uint32_t gw = size.w;
    uint32_t gh = size.h;
    float wv[16];
    float wvp[16];
    MatMul(s.transforms[kTsWorld].data(), s.transforms[kTsView].data(), wv);
    MatMul(wv, s.transforms[kTsProjection].data(), wvp);
    const auto& v = s.viewport;
    float sx = float(v.w) / gw;
    float sy = float(v.h) / gh;
    float ox = (2.0f * v.x + v.w) / gw - 1;
    float oy = 1 - (2.0f * v.y + v.h) / gh;
    float vpm[16] = {sx, 0, 0, 0, 0, sy, 0, 0, 0, 0, v.max_z - v.min_z, 0, ox, oy, v.min_z, 1};
    float full[16];
    MatMul(wvp, vpm, full);
    Transpose(full, c.wvp);
    Transpose(wv, c.wv);
    float n[16] = {};
    InverseTranspose3x3(wv, n);
    Transpose(n, c.wvn);
    for (int t = 0; t < 4; t++) Transpose(s.transforms[kTsTexture0 + size_t(t)].data(), c.tm[t]);
}

constexpr int kLightFloats = 26;
void LightToView(const std::array<uint32_t, kLightFloats>& l, const float* vm, float (&o)[7][4]) {
    float f[kLightFloats];
    std::memcpy(f, l.data(), sizeof(f));
    for (int j = 0; j < 4; j++) {
        o[0][j] = f[1 + j];
        o[1][j] = f[5 + j];
        o[2][j] = f[9 + j];
    }
    const float* pos = f + 13;
    const float* dir = f + 16;
    for (int j = 0; j < 3; j++) {
        o[3][j] = pos[0] * vm[j] + pos[1] * vm[4 + j] + pos[2] * vm[8 + j] + vm[12 + j];
        o[4][j] = dir[0] * vm[j] + dir[1] * vm[4 + j] + dir[2] * vm[8 + j];
    }
    o[3][3] = f[19];
    o[4][3] = f[20];
    o[5][0] = f[21];
    o[5][1] = f[22];
    o[5][2] = f[23];
    o[6][0] = std::cos(f[24] * 0.5f);
    o[6][1] = std::cos(f[25] * 0.5f);
}

void FillLightConstants(const State& s, const FixedKey& fk, DrawConstants& c) {
    std::memcpy(c.mat, s.material.data(), sizeof(float) * 16);
    c.mat[4][0] = s.material[16];
    Color(s.rs[kRsAmbient], c.amb);
    const float* vm = s.transforms[kTsView].data();
    for (int i = 0; i < 8; i++) {
        if (!((fk.types >> (i * 2)) & 3)) continue;
        LightToView(s.lights[size_t(i)], vm, c.lt[i]);
    }
}

struct WRange {
    float near_w;
    float far_w;
};

WRange ProjectionWRange(const float* m) {
    float m33 = m[10], m34 = m[11], m43 = m[14], m44 = m[15];
    if (m33 == m34 || m33 == 0) return {0, 1};
    return {m44 - m43 / m33 * m34, (m44 - m43) / (m33 - m34) * m34 + m44};
}

constexpr uint32_t kMClipMin = 0x394;
constexpr uint32_t kMClipMax = 0x398;
constexpr float kFallbackNear = 1.0f / 65536;

WRange DrawWRange(const State& s, bool program) {
    WRange r{0, s.zscale};
    if (!program) r = ProjectionWRange(s.transforms[kTsProjection].data());
    else if (SimpleState(s, kMClipMax)) r.near_w = Bits(SimpleState(s, kMClipMin));
    if (r.near_w <= 0 || r.near_w >= r.far_w) r.near_w = r.far_w * kFallbackNear;
    return r;
}

void VulkanBackend::FillDepthConstants(const State& s, const DrawShaders& sh, const PsoKey& k, DrawConstants& c) const {
    bool offset = OffsetsPolygons(s, Topology(k.topo));
    float units = offset ? Bits(SimpleState(s, kMDepthOffset)) : 0;
    if (s.rs[kRsZEnable] != kZUseW) {
        c.zparams[2] = units / s.zscale;
        return;
    }
    WRange r = DrawWRange(s, sh.prog);
    c.zparams[0] = r.near_w / (r.far_w - r.near_w);
    c.zparams[1] = r.far_w;
    c.zparams[2] = sh.prog ? units : units * r.far_w / s.zscale;
}

void VulkanBackend::UploadConstants(const State& s, const DrawShaders& sh, const PsoKey& k, const DrawTextures& stages,
                                    GuestExtent size, DrawConstants& c, Descriptors& set) {
    bool same = last_vsc_alloc_.buf && !std::memcmp(last_vsc_.data(), s.vsc.data(), sizeof(s.vsc));
    if (!same) {
        last_vsc_alloc_ = Upload(sizeof(s.vsc), 256);
        std::memcpy(last_vsc_alloc_.cpu, s.vsc.data(), sizeof(s.vsc));
        std::memcpy(last_vsc_.data(), s.vsc.data(), sizeof(s.vsc));
    }
    FillSurfaceConstants(s, size, c);
    if (k.topo == uint32_t(Topology::Points)) c.surf[3] = TargetScale();
    FillFogConstants(s, sh.prog, c);
    FillCombinerConstants(s, c);
    FillWideConstants(c);
    FillDepthConstants(s, sh, k, c);
    FillTextureConstants(s, sh, stages, c);
    if (sh.fixed_transform) {
        FillTransformConstants(s, size, c);
        FillLightConstants(s, sh.fk, c);
    }
    if (!last_dc_alloc_.buf || std::memcmp(&last_dc_, &c, sizeof(c))) {
        last_dc_alloc_ = Upload(sizeof(DrawConstants), 256);
        std::memcpy(last_dc_alloc_.cpu, &c, sizeof(c));
        last_dc_ = c;
    }
    set.ubo[0] = last_vsc_alloc_.buf;
    set.off[0] = last_vsc_alloc_.off;
    set.range[0] = sizeof(s.vsc);
    set.ubo[1] = last_dc_alloc_.buf;
    set.off[1] = last_dc_alloc_.off;
    set.range[1] = sizeof(DrawConstants);
}

bool VulkanBackend::SkippedByTexture(const State& s) {
    const auto& skip = diag::SkipTextures();
    if (skip.empty() && !app::kGame.skip_texture) return false;
    for (int i = 0; i < 4; i++) {
        TextureDesc d;
        if (!s.textures[size_t(i)] || !Describe(s.textures[size_t(i)], d)) continue;
        if (app::kGame.skip_texture && app::kGame.skip_texture(d.data)) return true;
        for (uint32_t t : skip)
            if ((d.data & 0x0FFFFFFF) == (t & 0x0FFFFFFF)) return true;
    }
    return false;
}

void VulkanBackend::CaptureTextures(const State& s, const DrawTextures& stages) {
    for (int i = 0; i < 4; i++) {
        Image* res = stages.res[i];
        TextureDesc d;
        if (!s.textures[size_t(i)] || !Describe(s.textures[size_t(i)], d)) continue;
        bool tg = targets_.count(SizedTargetKey(d.data, Format(d.format).depth, d.width, d.height));
        fprintf(cap_, " | t%d %08X fmt %X %ux%u%s%s", i, d.data, d.format, d.width, d.height, tg ? " target" : "",
                res ? "" : " unbound");
        if (res && !tg && stages.info[i].kind == kTex2D && !IsBc(res->fmt) && cap_seen_.insert(d.data).second) {
            ImageRef keep;
            auto it = textures_.find(d.data);
            if (it != textures_.end()) keep = it->second.img;
            cap_tex_.push_back({res, keep, d.data, d.width, d.height});
        }
    }
}

constexpr int kVsConstants = 192;
constexpr int kVsConstantBias = 96;

void VulkanBackend::CaptureInputs(const State& s, const Assembled& a, const DrawConstants& c) {
    fprintf(cap_, "\n     verts");
    uint32_t n = 0;
    for (int r = 0; r < 16; r++) n += (a.regs >> r) & 1;
    for (size_t v = 0; v < std::min<size_t>(4, n ? a.vertices.size() / (n * 4) : 0); v++) {
        fprintf(cap_, " [");
        for (uint32_t k = 0; k < n * 4; k++) fprintf(cap_, " %g", a.vertices[v * n * 4 + k]);
        fprintf(cap_, " ]");
    }
    for (int r = 0; r < kVsConstants; r++) {
        const auto& v = s.vsc[r];
        if (v[0] || v[1] || v[2] || v[3]) fprintf(cap_, "\n     c%d %g %g %g %g", r - kVsConstantBias, v[0], v[1], v[2], v[3]);
    }
    fprintf(cap_, "\n     pc");
    for (size_t r = 0; r < std::size(c.pc); r++)
        fprintf(cap_, " (%.3f %.3f %.3f %.3f)", c.pc[r][0], c.pc[r][1], c.pc[r][2], c.pc[r][3]);
}

void VulkanBackend::CaptureDraw(const State& s, uint32_t prim, uint32_t count, const PsoKey& k, const DrawConstants& c,
                                const DrawTextures& stages) {
    fprintf(cap_,
            "%04u draw rt %08X ds %08X prim %u count %u vs %08X ps %08X blend %u %X %X zen %u %X zwrite %u "
            "zbias %d %.2f cull %u cw %X c0 %.3f %.3f %.3f %.3f",
            cap_i_++, s.rt.data, bound_ds_ ? s.zs.data : 0, prim, count, s.vs_handle, s.ps ? s.ps->handle : 0, k.blend,
            k.src, k.dst, k.zen, k.zfunc, k.zwrite, k.zbias, k.zslope, k.cull, k.wmask, c.pc[0][0], c.pc[0][1],
            c.pc[0][2], c.pc[0][3]);
    if (c.wide[1] != 0) fprintf(cap_, " | wide x %g..%g side %g%s", draw_x_[0], draw_x_[1], c.wide[2], c.wide[3] ? "" : " (none)");
    CaptureTextures(s, stages);
}

void VulkanBackend::EndDrawCapture(const State& s, const Assembled& a, uint32_t count, const DrawConstants& c) {
    static const bool all = std::getenv("XBE_CAPTURE_ALL") != nullptr;
    if (all || count <= kQuadVertices || cap_vs_.insert(s.vs_handle).second) CaptureInputs(s, a, c);
    fputc('\n', cap_);
}

void VulkanBackend::CaptureShader(const char* kind, uint64_t key, const std::string& src) {
    if (!cap_sh_.insert(key).second) return;
    char name[48];
    snprintf(name, sizeof(name), "%s_%016llX.hlsl", kind, (unsigned long long)key);
    if (FILE* f = OpenFile(cap_dir_ / name, "w")) {
        fwrite(src.data(), 1, src.size(), f);
        fclose(f);
    }
    fprintf(cap_, "     %s\n", name);
}

void VulkanBackend::CaptureShaders(const State& s, const Assembled& a, const DrawShaders& sh, const PixelKey& pk) {
    std::string src = ReversedDepthHlsl(VertexHlsl(s, a, sh));
    CaptureShader("vs", sh.vkey, sh.quad ? QuadHlsl(std::move(src)) : src);
    CaptureShader("ps", sh.pkey, PixelHlsl(pk));
}

bool UnitTexcoords(const Assembled& a) {
    constexpr float kLo = -1e-4f;
    constexpr float kHi = 1.0001f;
    constexpr uint32_t kSmallDrawVertices = 4096;
    if (!a.vertices.empty() || !a.vertex_count || a.vertex_count > kSmallDrawVertices) return false;
    bool any = false;
    for (int r = 1; r < 16; r++) {
        const VertexElement& e = a.elements[size_t(r)];
        if (!(a.regs >> r & 1) || e.format != DXGI_FORMAT_R32G32_FLOAT) continue;
        const VertexBuffer& vb = a.buffers[e.buffer];
        if (!vb.src || !vb.stride || uint64_t(a.vertex_count) * vb.stride > vb.bytes) return false;
        for (uint32_t v = 0; v < a.vertex_count; v++) {
            float uv[2];
            std::memcpy(uv, vb.src + size_t(v) * vb.stride + e.offset, sizeof(uv));
            if (!(uv[0] >= kLo && uv[0] <= kHi && uv[1] >= kLo && uv[1] <= kHi)) return false;
        }
        any = true;
    }
    return any;
}

bool AnyStageWraps(const State& s) {
    for (const auto& t : s.tss)
        if (t[kTssAddressU] == kTaddressWrap || t[kTssAddressV] == kTaddressWrap) return true;
    return false;
}

void VulkanBackend::SetTextureViewsAndSamplers(const State& s, const Assembled& a, DrawTextures& stages,
                                               Descriptors& set) {
    for (int i = 0; i < 4; i++) {
        if (stages.res[i]) {
            set.view[i] = stages.res[i]->view;
            continue;
        }
        uint8_t kind = stages.info[i].kind;
        set.view[i] = (kind == kTexCube ? dummy_[2] : kind == kTex3D ? dummy_[1] : dummy_[0])->view;
    }
    bool fixed = !a.pretransformed && !((s.vs_handle & 1) && s.vs && !s.vs->program.empty());
    bool clamp = AnyStageWraps(s) && UnitTexcoords(a);
    uint32_t aniso = app::Get().aniso;
    for (int i = 0; i < 4; i++) set.smp[i] = StageSampler(s, i, stages, clamp, fixed, aniso);
}

void VulkanBackend::SetViewportScissor(const State& s, uint32_t gw) {
    float W = float(bound_rt_ ? bound_rt_->w : bound_ds_->w);
    float H = float(bound_rt_ ? bound_rt_->h : bound_ds_->h);
    VkViewport vp = D3dViewport(0, 0, W, H);
    float sc = W / gw;
    int32_t x0 = int32_t(s.viewport.x * sc);
    int32_t y0 = int32_t(s.viewport.y * sc);
    int32_t x1 = int32_t((s.viewport.x + s.viewport.w) * sc);
    int32_t y1 = int32_t((s.viewport.y + s.viewport.h) * sc);
    if (s.scissor_count) {
        x0 = std::max(x0, int32_t(s.scissors[0] * sc));
        y0 = std::max(y0, int32_t(s.scissors[1] * sc));
        x1 = std::min(x1, int32_t(s.scissors[2] * sc));
        y1 = std::min(y1, int32_t(s.scissors[3] * sc));
    }
    x0 = std::clamp<int32_t>(x0, 0, int32_t(W));
    y0 = std::clamp<int32_t>(y0, 0, int32_t(H));
    x1 = std::clamp<int32_t>(x1, 0, int32_t(W));
    y1 = std::clamp<int32_t>(y1, 0, int32_t(H));
    VkRect2D rect{{x0, y0}, {uint32_t(std::max(x1 - x0, 0)), uint32_t(std::max(y1 - y0, 0))}};
    bool same = binds_.rasterizer && !std::memcmp(&vp, &binds_.viewport, sizeof(vp)) &&
                !std::memcmp(&rect, &binds_.scissor, sizeof(rect));
    if (same) return;
    vkCmdSetViewport(cb_, 0, 1, &vp);
    vkCmdSetScissor(cb_, 0, 1, &rect);
    binds_.rasterizer = true;
    binds_.viewport = vp;
    binds_.scissor = rect;
}

void VulkanBackend::SetBlendFactorStencilRef(const State& s) {
    float bf[4];
    Color(SimpleState(s, kMBlendColor), bf);
    uint32_t ref = SimpleState(s, kMStencilRef) & 0xFF;
    if (binds_.output && !std::memcmp(bf, binds_.blend, sizeof(bf)) && ref == binds_.stencil) return;
    vkCmdSetBlendConstants(cb_, bf);
    vkCmdSetStencilReference(cb_, VK_STENCIL_FACE_FRONT_AND_BACK, ref);
    binds_.output = true;
    std::memcpy(binds_.blend, bf, sizeof(bf));
    binds_.stencil = ref;
}

VkPrimitiveTopology PrimitiveTopology(const Assembled& a) {
    if (a.topology == Topology::Points) return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
    if (a.topology == Topology::Lines) return a.strip ? VK_PRIMITIVE_TOPOLOGY_LINE_STRIP : VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
    return a.strip ? VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
}

void VulkanBackend::DrawGeometry(const Assembled& a) {
    VkBuffer bufs[16];
    VkDeviceSize offs[16], strides[16];
    for (uint32_t i = 0; i < a.buffer_count; i++) {
        const VertexBuffer& b = a.buffers[i];
        Alloc vb = Upload(std::max<uint32_t>(b.bytes, 4), 16);
        if (b.bytes) std::memcpy(vb.cpu, b.src, b.bytes);
        bufs[i] = vb.buf;
        offs[i] = vb.off;
        strides[i] = b.stride;
    }
    if (a.buffer_count) vkCmdBindVertexBuffers2(cb_, 0, a.buffer_count, bufs, offs, nullptr, strides);
    vkCmdSetPrimitiveTopology(cb_, PrimitiveTopology(a));
    if (a.index_count) {
        uint32_t ibytes = a.index_count * (a.index16 ? 2 : 4);
        Alloc ib = Upload(ibytes, 4);
        std::memcpy(ib.cpu, a.index_src, ibytes);
        vkCmdBindIndexBuffer(cb_, ib.buf, ib.off, a.index16 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(cb_, a.index_count, 1, 0, a.base_vertex, 0);
    } else {
        vkCmdDraw(cb_, a.draw_count, 1, 0, 0);
    }
}

bool VulkanBackend::AssembleDraw(const State& s, uint32_t prim, uint32_t start, uint32_t count,
                                 const uint8_t* indices, Assembled& a) {
    bool assembled;
    {
        PerfScope perf_assemble{g_perf.assemble};
        assembled = Assemble(s, prim, start, count, indices, a);
    }
    g_perf.verts += a.vertex_count;
    g_perf.raw += a.vertices.empty();
    return assembled;
}

bool VulkanBackend::EnsureQueries() {
    if (query_pool_) return true;
    if (queries_failed_) return false;
    VkQueryPoolCreateInfo ci{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    ci.queryType = VK_QUERY_TYPE_OCCLUSION;
    ci.queryCount = kQueries;
    if (vkCreateQueryPool(dev_, &ci, nullptr, &query_pool_) != VK_SUCCESS) {
        queries_failed_ = true;
        LOG_FIRST(1, "vulkan: no occlusion queries, visibility tests stay pending");
        return false;
    }
    vkResetQueryPool(dev_, query_pool_, 0, kQueries);
    return true;
}

uint32_t VulkanBackend::BeginCountedDraw(const State& s) {
    if (!s.simple[kMCountEnable >> 2] || !EnsureQueries()) return kNoQuery;
    uint32_t query = query_next_++ % kQueries;
    // reset on the host: the ring is far longer than the queries a few frames have in flight
    vkResetQueryPool(dev_, query_pool_, query, 1);
    vkCmdBeginQuery(cb_, query_pool_, query, VK_QUERY_CONTROL_PRECISE_BIT);
    return query;
}

void VulkanBackend::EndCountedDraw(uint32_t query) {
    if (query == kNoQuery) return;
    vkCmdEndQuery(cb_, query_pool_, query);
    counted_.push_back({query, SamplesPerPixel()});
}

float VulkanBackend::SamplesPerPixel() const {
    const VTarget* t = bound_rt_ ? bound_rt_ : bound_ds_;
    if (!t || !t->gw || !t->gh) return 1;
    return float(t->w) * float(t->h) / (float(t->gw) * float(t->gh));
}

void VulkanBackend::DeliverReports() {
    constexpr uint32_t kTime = 0, kTimeHigh = 4, kCount = 8, kStatus = 12;
    uint64_t done = Completed();
    while (!reports_.empty() && reports_.front().fence <= done) {
        const Report& r = reports_.front();
        double pixels = 0;
        for (const Query& q : r.queries) {
            uint64_t n = 0;
            vkGetQueryPoolResults(dev_, query_pool_, q.index, 1, sizeof(n), &n, sizeof(n), VK_QUERY_RESULT_64_BIT);
            pixels += double(n) / q.samples_per_pixel;
        }
        kernel::W32(r.va + kTime, uint32_t(presents_));
        kernel::W32(r.va + kTimeHigh, 0);
        kernel::W32(r.va + kCount, uint32_t(std::lround(pixels)));
        std::atomic_thread_fence(std::memory_order_release);
        kernel::W32(r.va + kStatus, 0);
        reports_.pop_front();
    }
}

constexpr uint32_t kPointList = 1;
constexpr uint32_t kTopDisable = 1;
constexpr int kSpriteStage = 3;

bool SpriteOnStage3(const State& s, uint32_t prim) {
    bool prog = (s.vs_handle & 1) && s.vs && !s.vs->program.empty();
    bool stage3 = s.tss[kSpriteStage][kTssColorOp] != kTopDisable;
    return prim == kPointList && prog && !s.ps && s.rs[kRsPointSpriteEnable] && stage3;
}

State SpriteStageZero(const State& s) {
    State moved = s;
    moved.tss[0] = s.tss[kSpriteStage];
    moved.textures[0] = s.textures[kSpriteStage];
    moved.palettes[0] = s.palettes[kSpriteStage];
    for (int i = 1; i < 4; i++) {
        moved.tss[i][kTssColorOp] = kTopDisable;
        moved.tss[i][kTssAlphaOp] = kTopDisable;
        moved.textures[i] = 0;
    }
    return moved;
}

void VulkanBackend::Draw(const State& s, uint32_t prim, uint32_t start, uint32_t count, const uint8_t* indices) {
    if (SpriteOnStage3(s, prim)) {
        Draw(SpriteStageZero(s), prim, start, count, indices);
        return;
    }
    PerfScope perf_draw{g_perf.draw};
    g_perf.draws++;
    if (cap_) CapStep(s.rt);
    if (DepthCopy(s)) return;
    Assembled& a = asm_;
    if (!AssembleDraw(s, prim, start, count, indices, a)) return;
    if (CurrentFrame().used > kUploadSize - kUploadReserve) Submit(true);
    if (!BindTargets(s)) return;

    DrawShaders sh = ClassifyShaders(s, a, count);
    if (sh.prog) EnsureProgramHash(s);
    sh.vkey = VertexKey(s, a, sh);
    EnsureVertexShader(s, a, sh);

    DrawTextures stages;
    bool writes_depth = s.rs[kRsZEnable] != 0 && SimpleState(s, kMZWriteEnable) != 0;
    MeasureDrawX(a, writes_depth);
    draw_ui_ = DrawIs2D(s, a, writes_depth);
    if (!BindStageTexturesAndTargets(s, stages)) return;
    PixelKey pk = MakePixelKey(s, stages.info);
    pk.quad = sh.quad;
    sh.pkey = Hash(&pk, sizeof(pk));
    EnsurePixelShader(pk, sh);
    if (!sh.vs_code || !sh.ps_code) return;

    PsoKey k;
    BuildPipelineKey(s, a, sh.vh, sh.ph, k);
    k.sprite = sh.sprite;
    VkPipeline pso = EnsurePipeline(k, sh.vs_code, sh.ps_code);
    if (!pso) return;

    GuestExtent size;
    SurfaceSize(bound_rt_ ? s.rt : s.zs, size.w, size.h);
    DrawConstants c{};
    Descriptors set;
    UploadConstants(s, sh, k, stages, size, c, set);

    if (cap_) {
        CaptureDraw(s, prim, count, k, c, stages);
        EndDrawCapture(s, a, count, c);
        CaptureShaders(s, a, sh, pk);
    }
    if (SkippedByTexture(s)) return;

    SetTextureViewsAndSamplers(s, a, stages, set);
    EnsureRendering();
    if (pso != binds_.pso) vkCmdBindPipeline(cb_, VK_PIPELINE_BIND_POINT_GRAPHICS, pso);
    binds_.pso = pso;
    PushDescriptors(set);
    SetViewportScissor(s, size.w);
    SetBlendFactorStencilRef(s);
    uint32_t query = BeginCountedDraw(s);
    DrawGeometry(a);
    EndCountedDraw(query);
}

// Copies

constexpr uint32_t kGuestRectBytes = 16;
constexpr uint32_t kGuestPointBytes = 8;

struct GuestRect {
    int32_t left, top, right, bottom;
};
struct GuestPoint {
    int32_t x, y;
};

GuestRect ReadGuestRect(uint32_t rects, uint32_t i) {
    uint32_t r = rects + i * kGuestRectBytes;
    return {int32_t(kernel::R32(r)), int32_t(kernel::R32(r + 4)), int32_t(kernel::R32(r + 8)), int32_t(kernel::R32(r + 12))};
}

GuestPoint ReadGuestPoint(uint32_t points, uint32_t i) {
    uint32_t p = points + i * kGuestPointBytes;
    return {int32_t(kernel::R32(p)), int32_t(kernel::R32(p + 4))};
}

struct DrawnRect {
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0, dx = 0, dy = 0;
};

DrawnRect ReadDrawnRect(uint32_t src_rects, uint32_t dst_points, uint32_t i) {
    GuestRect g = ReadGuestRect(src_rects, i);
    DrawnRect r{float(g.left), float(g.top), float(g.right), float(g.bottom), float(g.left), float(g.top)};
    if (dst_points) {
        GuestPoint p = ReadGuestPoint(dst_points, i);
        r.dx = float(p.x);
        r.dy = float(p.y);
    }
    return r;
}

uint32_t ScaleUnsigned(int32_t v, float sc) { return uint32_t(uint32_t(v) * sc); }
constexpr float kTapsSlack = 1e-3f;

void VulkanBackend::CopyByDraw(const VTarget& src_t, VTarget& dst_t, const CopyList& l) {
    Transition(*src_t.img, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    want_ = {dst_t.img.get(), nullptr};
    float gw = src_t.gw * l.kx, gh = src_t.gh * l.ky;
    float src_scale = float(src_t.w) / gw;
    float dst_scale = float(dst_t.w) / dst_t.gw;
    bool listed = l.count && l.src_rects;
    uint32_t n = listed ? l.count : 1;
    for (uint32_t i = 0; i < n; i++) {
        float whole_w = std::min(gw, float(dst_t.gw));
        float whole_h = std::min(gh, float(dst_t.gh));
        DrawnRect r{.x0 = 0, .y0 = 0, .x1 = whole_w, .y1 = whole_h};
        if (listed) r = ReadDrawnRect(l.src_rects, l.dst_points, i);
        if (r.x1 <= r.x0 || r.y1 <= r.y0) continue;
        float vx = r.dx * dst_scale, vy = r.dy * dst_scale, vw = (r.x1 - r.x0) * dst_scale, vh = (r.y1 - r.y0) * dst_scale;
        int32_t right = std::min(int32_t(vx + vw), int32_t(dst_t.w));
        int32_t bottom = std::min(int32_t(vy + vh), int32_t(dst_t.h));
        if (right <= int32_t(vx) || bottom <= int32_t(vy)) continue;
        VkViewport vp = D3dViewport(vx, vy, vw, vh);
        VkRect2D scissor{{int32_t(vx), int32_t(vy)}, {uint32_t(right - int32_t(vx)), uint32_t(bottom - int32_t(vy))}};
        vkCmdSetViewport(cb_, 0, 1, &vp);
        vkCmdSetScissor(cb_, 0, 1, &scissor);
        binds_ = {};
        float uv[4] = {r.x0 / gw, r.y0 / gh, (r.x1 - r.x0) / gw, (r.y1 - r.y0) / gh};
        uint32_t taps = uint32_t(std::ceil(std::max(src_scale / dst_scale, 1.0f) - kTapsSlack));
        BlitOptions o{.alpha = true, .taps = taps, .rtv = dst_t.fmt};
        Blit(*src_t.img, uv, o);
    }
    dst_t.drawn = true;
    bound_rt_ = bound_ds_ = nullptr;
    want_ = {};
}

void VulkanBackend::CopyByRegion(const VTarget& src_t, VTarget& dst_t, const CopyList& l) {
    Transition(*src_t.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    Transition(*dst_t.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    float dst_scale = float(dst_t.w) / dst_t.gw;
    bool listed = l.count && l.src_rects;
    uint32_t n = listed ? l.count : 1;
    std::vector<VkImageCopy> regions;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t left = 0, top = 0, right = std::min(src_t.w, dst_t.w), bottom = std::min(src_t.h, dst_t.h);
        uint32_t dx = 0, dy = 0;
        if (listed) {
            GuestRect r = ReadGuestRect(l.src_rects, i);
            left = ScaleUnsigned(r.left, dst_scale);
            top = ScaleUnsigned(r.top, dst_scale);
            right = std::min(ScaleUnsigned(r.right, dst_scale), src_t.w);
            bottom = std::min(ScaleUnsigned(r.bottom, dst_scale), src_t.h);
            dx = left;
            dy = top;
            if (l.dst_points) {
                GuestPoint p = ReadGuestPoint(l.dst_points, i);
                dx = ScaleUnsigned(p.x, dst_scale);
                dy = ScaleUnsigned(p.y, dst_scale);
            }
        }
        if (right <= left || bottom <= top || dx >= dst_t.w || dy >= dst_t.h) continue;
        right = std::min(right, left + dst_t.w - dx);
        bottom = std::min(bottom, top + dst_t.h - dy);
        VkImageCopy c{};
        c.srcSubresource = c.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        c.srcOffset = {int32_t(left), int32_t(top), 0};
        c.dstOffset = {int32_t(dx), int32_t(dy), 0};
        c.extent = {right - left, bottom - top, 1};
        regions.push_back(c);
    }
    if (!regions.empty())
        vkCmdCopyImage(cb_, src_t.img->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst_t.img->img,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, uint32_t(regions.size()), regions.data());
    dst_t.drawn = true;
    bound_rt_ = bound_ds_ = nullptr;
    want_ = {};
}

void VulkanBackend::Copy(const Surface& src, const Surface& dst, uint32_t count, uint32_t src_rects,
                         uint32_t dst_points, float src_kx, float src_ky) {
    PerfScope perf_copy{g_perf.copy};
    VTarget* src_t = FindTarget(src, false);
    if (cap_) {
        const char* from = "memory";
        if (src_t) from = src_t->drawn ? "target" : "idle target";
        fprintf(cap_, "%04u copy %08X %s to %08X\n", cap_i_++, src.data, from, dst.data);
    }
    if (!src_t || !src_t->drawn) {
        if (!CopySurfaceMemory(src, dst, count, src_rects, dst_points)) {
            LOG_FIRST(8, "vulkan: CopyRects %08X to %08X skipped", src.data, dst.data);
        } else if (VTarget* dst_t = FindTarget(dst, false)) {
            UploadTarget(*dst_t, dst);
        }
        return;
    }
    VTarget* dst_t = EnsureTarget(dst, false);
    if (!dst_t || src_t == dst_t) return;
    CopyList l{.count = count, .src_rects = src_rects, .dst_points = dst_points, .kx = src_kx, .ky = src_ky};
    if (src_t->fmt != dst_t->fmt || src_kx != 1 || src_ky != 1) {
        CopyByDraw(*src_t, *dst_t, l);
        return;
    }
    CopyByRegion(*src_t, *dst_t, l);
    if (cap_) {
        char name[32];
        snprintf(name, sizeof(name), "%04u_copy_%08X", cap_i_, dst_t->data);
        CapSave(*dst_t, name);
    }
}

// Present

void VulkanBackend::ResizeSwap() {
    int pw = 0, ph = 0;
    WindowPixels(pw, ph);
    uint32_t n = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(phys_, surface_, &n, nullptr);
    std::vector<VkPresentModeKHR> modes(n);
    vkGetPhysicalDeviceSurfacePresentModesKHR(phys_, surface_, &n, modes.data());
    bool resized = pw > 0 && ph > 0 && (uint32_t(pw) != bb_w_ || uint32_t(ph) != bb_h_);
    if (!resized && !swap_lost_ && WantedMode(modes) == swap_mode_) return;
    Flush();
    swap_lost_ = !CreateSwapchain();
}

void VulkanBackend::FinishCapture(const Surface& front) {
    CapStep(Surface{});
    if (VTarget* f = FindTarget(front, false)) CapSave(*f, "final");
    for (auto& t : cap_tex_) {
        char name[32];
        snprintf(name, sizeof(name), "tex_%08X", t.data);
        CapSave(*t.img, t.w, t.h, 1, name);
    }
    fclose(cap_);
    cap_ = nullptr;
    shown_dir_ = cap_dir_;
    cap_tex_.clear();
    LOGI("vulkan: frame captured to %s", cap_dir_.string().c_str());
}

void VulkanBackend::StartCapture() {
    std::time_t now = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &now);
#else
    localtime_r(&now, &tm);
#endif
    char stamp[32];
    snprintf(stamp, sizeof(stamp), "%02d%02d%02d", tm.tm_hour, tm.tm_min, tm.tm_sec);
    std::error_code ec;
    cap_dir_ = std::filesystem::weakly_canonical(app::DataDir() / diag::CaptureDir() / stamp, ec);
    std::filesystem::create_directories(cap_dir_, ec);
    cap_ = OpenFile(cap_dir_ / "frame.txt", "w");
    cap_i_ = cap_rt_ = 0;
    cap_seen_.clear();
    cap_vs_.clear();
    cap_sh_.clear();
}

void VulkanBackend::UploadGammaRamp() {
    ramp_dirty_ = false;
    std::vector<uint8_t> px(256 * 4);
    bool identity = true;
    for (int i = 0; i < 256; i++) {
        px[i * 4 + 0] = ramp_[512 + i];
        px[i * 4 + 1] = ramp_[256 + i];
        px[i * 4 + 2] = ramp_[i];
        px[i * 4 + 3] = 255;
        identity &= ramp_[i] == i && ramp_[256 + i] == i && ramp_[512 + i] == i;
    }
    gamma_ = !identity;
    UploadTextureLevel(*lut_, 0, 0, px, 256 * 4);
    Transition(*lut_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

void VulkanBackend::BlitFrame(VTarget& t) {
    float aspect = app::FrameAspect();
    float w = float(bb_w_);
    float h = float(bb_h_);
    float vw = std::min(w, h * aspect);
    float vh = vw / aspect;
    float uv[4] = {0, 0, 1, 1};
    if (app::kGame.view_w && t.gw) uv[2] = std::min(1.0f, float(app::kGame.view_w) / t.gw);
    if (app::kGame.view_h && t.gh) uv[3] = std::min(1.0f, float(app::kGame.view_h) / t.gh);
    // whole pixels: FSR's passes address the view by pixel; x, y, width, height as D3D's viewport has them
    VkViewport vp{std::floor((w - vw) / 2), std::floor((h - vh) / 2), std::round(vw), std::round(vh), 0, 1};
    if (PresentPost(t, uv, vp)) return;
    Image& bb = *bb_[present_image_];
    Transition(*t.img, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    want_ = {&bb, nullptr};
    VkViewport dv = D3dViewport(vp.x, vp.y, vp.width, vp.height);
    VkRect2D scissor{{0, 0}, {bb_w_, bb_h_}};
    vkCmdSetViewport(cb_, 0, 1, &dv);
    vkCmdSetScissor(cb_, 0, 1, &scissor);
    binds_ = {};
    Blit(*t.img, uv, {.gamma = gamma_, .rtv = swap_fmt_});
}

void VulkanBackend::PresentSwap(uint32_t bi) {
    auto now = PerfClock::now();
    last_swap_ = now;
    VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &rendered_[bi];
    pi.swapchainCount = 1;
    pi.pSwapchains = &swap_;
    pi.pImageIndices = &bi;
    VkResult r = vkQueuePresentKHR(queue_, &pi);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) swap_lost_ = true;
}

bool VulkanBackend::CaptureDue() {
    const auto& at = diag::CaptureAt();
    if (capture_at_next_ >= at.size()) return false;
    double t = std::chrono::duration<double>(PerfClock::now() - run_start_).count();
    if (t < at[capture_at_next_]) return false;
    capture_at_next_++;
    return true;
}

void VulkanBackend::WriteFrameTime() {
    if (diag::FrameTimesFile().empty()) return;
    if (!frame_times_) {
        frame_times_ = OpenFile(diag::FrameTimesFile(), "w");
        if (!frame_times_) return;
        fputs("present,time_ms,interval_ms,shaders,pipelines,tex_uploads,new_textures,read_backs,new_targets\n",
              frame_times_);
        last_present_ = PerfClock::now();
        LOGR("Frame times: frame clock %.3f ms", std::chrono::duration<double, std::milli>(last_present_ - run_start_).count());
    }
    auto now = PerfClock::now();
    double t = std::chrono::duration<double, std::milli>(now - run_start_).count();
    double dt = std::chrono::duration<double, std::milli>(now - last_present_).count();
    last_present_ = now;
    uint64_t counts[6] = {g_perf.shaders, g_perf.psos, g_perf.texups, g_perf.newtex, g_perf.syncs, g_perf.targets};
    unsigned long long d[6];
    for (int i = 0; i < 6; i++) {
        d[i] = counts[i] >= frame_counts_[i] ? counts[i] - frame_counts_[i] : counts[i];
        frame_counts_[i] = counts[i];
    }
    fprintf(frame_times_, "%llu,%.3f,%.3f,%llu,%llu,%llu,%llu,%llu,%llu\n", (unsigned long long)presents_, t, dt, d[0],
            d[1], d[2], d[3], d[4], d[5]);
    if (presents_ % 60 == 0) fflush(frame_times_);
}

bool VulkanBackend::EnsurePresentPipelines() {
    if (easu_pso_) return true;
    if (present_failed_) return false;
    present_failed_ = true;
    auto module = [&](const uint32_t* code, size_t bytes) {
        std::vector<uint32_t> v(code, code + bytes / 4);
        return Module(v);
    };
    VkShaderModule vs = module(kPresentVsSpv, sizeof(kPresentVsSpv));
    VkShaderModule easu = module(kPresentEasuSpv, sizeof(kPresentEasuSpv));
    VkShaderModule fin = module(kPresentFinalSpv, sizeof(kPresentFinalSpv));
    // the present shaders' entry points keep their HLSL names: one pipeline each, the stage names set below
    auto make = [&](VkShaderModule ps, const char* entry, VkFormat fmt) -> VkPipeline {
        VkPipelineShaderStageCreateInfo st[2]{};
        st[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vs, "vs"};
        st[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, ps, entry};
        VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        vp.viewportCount = vp.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        rs.cullMode = VK_CULL_MODE_NONE;
        rs.lineWidth = 1;
        VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
        VkPipelineColorBlendAttachmentState att = NoBlend();
        VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        cb.attachmentCount = 1;
        cb.pAttachments = &att;
        VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        dy.dynamicStateCount = 2;
        dy.pDynamicStates = dyn;
        VkPipelineRenderingCreateInfo ri{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
        ri.colorAttachmentCount = 1;
        ri.pColorAttachmentFormats = &fmt;
        VkGraphicsPipelineCreateInfo pi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        pi.pNext = &ri;
        pi.stageCount = 2;
        pi.pStages = st;
        pi.pVertexInputState = &vi;
        pi.pInputAssemblyState = &ia;
        pi.pViewportState = &vp;
        pi.pRasterizationState = &rs;
        pi.pMultisampleState = &ms;
        pi.pDepthStencilState = &ds;
        pi.pColorBlendState = &cb;
        pi.pDynamicState = &dy;
        pi.layout = layout_;
        VkPipeline p = VK_NULL_HANDLE;
        vkCreateGraphicsPipelines(dev_, pcache_, 1, &pi, nullptr, &p);
        return p;
    };
    if (!vs || !easu || !fin) return false;
    easu_pso_ = make(easu, "ps_easu", kBgra);
    final_pso_ = make(fin, "ps_final", swap_fmt_);
    if (!easu_pso_ || !final_pso_) {
        LOGE("vulkan: present pipelines failed");
        easu_pso_ = VK_NULL_HANDLE;
        return false;
    }
    present_failed_ = false;
    return true;
}

bool VulkanBackend::EnsureMid(uint32_t w, uint32_t h) {
    if (mid_ && mid_->w == w && mid_->h == h) return true;
    if (mid_) CurrentFrame().garbage.push_back(mid_);
    mid_ = MakeImage(kBgra, w, h, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    return mid_ != nullptr;
}

bool VulkanBackend::EnsureColourLut() {
    const std::string& name = app::Get().colour_lut;
    if (name == clut_name_) return clut_ != nullptr;
    clut_name_ = name;
    if (clut_) CurrentFrame().garbage.push_back(clut_);
    clut_ = nullptr;
    clut_n_ = 0;
    if (name.empty()) return false;
    std::filesystem::path path = app::DataDir() / "luts" / std::filesystem::path(platform::Wide(name));
    std::ifstream f(path);
    if (!f) {
        LOGW("colour LUT %s not found", path.string().c_str());
        return false;
    }
    constexpr uint32_t kMaxSize = 128;
    uint32_t n = 0;
    std::vector<float> rgba;
    for (std::string line; std::getline(f, line);) {
        if (line.empty() || line[0] == '#') continue;
        if (line.rfind("LUT_3D_SIZE", 0) == 0) {
            n = uint32_t(std::atoi(line.c_str() + 11));
            if (n < 2 || n > kMaxSize) return false;
            rgba.reserve(size_t(n) * n * n * 4);
            continue;
        }
        float r, g, b;
        if (n && std::sscanf(line.c_str(), "%f %f %f", &r, &g, &b) == 3) rgba.insert(rgba.end(), {r, g, b, 1.0f});
    }
    if (!n || rgba.size() != size_t(n) * n * n * 4) {
        LOGW("colour LUT %s: not a 3D .cube table", path.string().c_str());
        return false;
    }
    clut_ = MakeImage(VK_FORMAT_R32G32B32A32_SFLOAT, n, n, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                      false, 1, n);
    if (!clut_) return false;
    std::vector<uint8_t> bytes(rgba.size() * 4);
    std::memcpy(bytes.data(), rgba.data(), bytes.size());
    UploadTextureLevel(*clut_, 0, 0, bytes, n * 16);
    Transition(*clut_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    clut_n_ = n;
    LOGI("colour LUT %s, %u^3", path.string().c_str(), n);
    return true;
}

bool VulkanBackend::PresentPost(VTarget& t, const float uv[4], const VkViewport& vp) {
    const app::Settings& st = app::Get();
    bool grade = st.grade_exposure || st.grade_contrast != 100 || st.grade_saturation != 100 || st.grade_temperature ||
                 st.grade_tint;
    float sw = t.w * uv[2], sh = t.h * uv[3];
    uint32_t dw = uint32_t(vp.width), dh = uint32_t(vp.height);
    bool upscale = st.upscaler == 1 && sw < dw && sh < dh;
    bool filter = st.colour_filter != 0 && st.colour_filter_strength != 0;
    bool clut = EnsureColourLut();
    if (!upscale && !st.sharpen && !grade && !filter && !clut) return false;
    if (!dw || !dh || !EnsurePresentPipelines() || !EnsureMid(dw, dh)) return false;
    Image* lut3d = clut_n_ ? clut_.get() : dummy_[1].get();

    // the view to mid_: EASU upscaling, else the plain filter
    SetBlitTarget(*mid_);
    Transition(*t.img, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    if (upscale) {
        AU1 con[16] = {};
        FsrEasuCon(con, con + 4, con + 8, con + 12, sw, sh, float(t.w), float(t.h), float(dw), float(dh));
        Alloc cb = Upload(256, 256);
        std::memcpy(cb.cpu, con, sizeof(con));
        Image* views[4] = {t.img.get(), lut_.get(), lut3d, nullptr};
        DrawFullScreen(easu_pso_, cb, views);
    } else {
        uint32_t taps = uint32_t(std::clamp(std::ceil(std::max(sw / dw, sh / dh)), 1.0f, 4.0f));
        Blit(*t.img, uv, {.taps = taps});
    }
    Transition(*mid_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    // mid_ to the swap chain: RCAS, the gamma ramp, the grade
    Image& bb = *bb_[present_image_];
    want_ = {&bb, nullptr};
    VkViewport dv = D3dViewport(vp.x, vp.y, vp.width, vp.height);
    VkRect2D scissor{{0, 0}, {bb_w_, bb_h_}};
    vkCmdSetViewport(cb_, 0, 1, &dv);
    vkCmdSetScissor(cb_, 0, 1, &scissor);
    struct {
        AU1 con[16];
        float opts[4], grade0[4], grade1[4], filter[4];
    } p{};
    constexpr float kSharpenStops = 2.0f;
    FsrRcasCon(p.con, kSharpenStops * (1.0f - st.sharpen / 100.0f));
    p.opts[0] = st.sharpen ? 1.0f : 0.0f;
    p.opts[1] = gamma_ ? 1.0f : 0.0f;
    p.opts[2] = vp.x;
    p.opts[3] = vp.y;
    p.grade0[0] = st.grade_exposure / 100.0f;
    p.grade0[1] = st.grade_contrast / 100.0f;
    p.grade0[2] = st.grade_saturation / 100.0f;
    p.grade0[3] = grade ? 1.0f : 0.0f;
    p.grade1[0] = st.grade_temperature / 100.0f;
    p.grade1[1] = st.grade_tint / 100.0f;
    p.filter[0] = filter ? float(st.colour_filter) : 0.0f;
    p.filter[1] = st.colour_filter_strength / 100.0f;
    p.filter[2] = clut ? 1.0f : 0.0f;
    p.filter[3] = float(clut_n_);
    Alloc cb = Upload(256, 256);
    std::memcpy(cb.cpu, &p, sizeof(p));
    Image* views[4] = {mid_.get(), lut_.get(), lut3d, nullptr};
    DrawFullScreen(final_pso_, cb, views);
    return true;
}

void VulkanBackend::RenderUi() {
    if (!ui_ready_) return;
    std::lock_guard lock(app::UiLock());
    ImGui_ImplVulkan_NewFrame();
    UiNewFrame();
    ImGui::NewFrame();
    if (app::kGame.ui) app::kGame.ui(fps_);
    ImGui::Render();
    ImDrawData* dd = ImGui::GetDrawData();
    if (!dd || !dd->CmdListsCount) return;
    want_ = {bb_[present_image_].get(), nullptr};
    EnsureRendering();
    ImGui_ImplVulkan_RenderDrawData(dd, cb_);
    binds_ = {};
}

void VulkanBackend::DrawFrame(VTarget* t, uint32_t bi) {
    Image& bb = *bb_[bi];
    // the swap chain image is the acquired one: its layout is new each time it comes back
    bb.layout = VK_IMAGE_LAYOUT_UNDEFINED;
    std::vector<VkRect2D> all{{{0, 0}, {bb_w_, bb_h_}}};
    VkClearValue black{};
    black.color.float32[3] = 1;
    ClearImage(bb, all, black);
    bound_rt_ = bound_ds_ = nullptr;
    if (t) BlitFrame(*t);
    RenderUi();
    Transition(bb, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
}

bool VulkanBackend::ReadShown(uint32_t bi, std::vector<uint8_t>& px) {
    Image& bb = *bb_[bi];
    VkImageLayout keep = bb.layout;
    bool ok = ReadBack(bb, bb_w_, bb_h_, px);
    Transition(bb, keep);
    if (ok) {
        // an RGBA swap chain to BGRA, the alpha opaque
        bool rgba = swap_fmt_ == VK_FORMAT_R8G8B8A8_UNORM;
        for (size_t i = 0; i < px.size(); i += 4) {
            if (rgba) std::swap(px[i], px[i + 2]);
            px[i + 3] = 0xFF;
        }
    }
    return ok;
}

void VulkanBackend::ReleaseOldTextures() {
    constexpr uint64_t kSweepEvery = 60;
    constexpr size_t kTexturesKept = 2048;
    constexpr uint64_t kIdleFrames = 600;
    if (presents_ % kSweepEvery != 0 || textures_.size() <= kTexturesKept) return;
    for (auto it = textures_.begin(); it != textures_.end();) {
        if (it->second.frame + kIdleFrames < presents_) {
            CurrentFrame().garbage.push_back(it->second.img);
            it = textures_.erase(it);
        } else {
            ++it;
        }
    }
}

void VulkanBackend::ResetFrame() {
    Frame& f = CurrentFrame();
    if (f.fence && Completed() < f.fence) {
        auto perf_wait = PerfClock::now();
        WaitTimeline(f.fence);
        g_perf.wait += PerfMs(perf_wait);
    }
    vkResetCommandPool(dev_, f.pool, 0);
    f.garbage.clear();
    f.garbage_buffers.clear();
    f.used = 0;
    Begin();
    FrameTimestamp(false);
}

// end false: the slot's last frame's GPU time into g_perf.gpu (its fence has passed), then this frame's start
void VulkanBackend::FrameTimestamp(bool end) {
    if (!ts_pool_ || !diag::Perf()) return;
    uint32_t slot = uint32_t(frame_ % kFrames) * 2;
    if (end) {
        vkCmdWriteTimestamp2(cb_, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, ts_pool_, slot + 1);
        return;
    }
    uint64_t t[2] = {};
    if (ts_written_[frame_ % kFrames] &&
        vkGetQueryPoolResults(dev_, ts_pool_, slot, 2, sizeof(t), t, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT) == VK_SUCCESS)
        g_perf.gpu += double(t[1] - t[0]) * props_.limits.timestampPeriod / 1e6;
    vkResetQueryPool(dev_, ts_pool_, slot, 2);
    vkCmdWriteTimestamp2(cb_, VK_PIPELINE_STAGE_2_NONE, ts_pool_, slot);
    ts_written_[frame_ % kFrames] = true;
}

void VulkanBackend::CountFps() {
    fps_n_++;
    auto now = std::chrono::steady_clock::now();
    double el = std::chrono::duration<double>(now - fps_t_).count();
    if (el >= 0.5) {
        fps_ = fps_n_ / el;
        fps_n_ = 0;
        fps_t_ = now;
    }
}

void LogPerf() {
    static PerfClock::time_point perf_start = PerfClock::now();
    static uint64_t perf_frames = 0;
    perf_frames++;
    double el = PerfMs(perf_start);
    if (el < 1000.0) return;
    Perf& p = g_perf;
    double n = double(perf_frames);
    double host = p.draw + p.clear + p.copy + p.present + p.sync;
    LOGR("perf: %.1f fps %.2f ms/frame | host %.2f (draw %.2f: assemble %.2f bind %.2f [check %.2f, upload %.2f] "
         "compile %.2f; clear %.2f copy %.2f sync %.2f present %.2f [swap %.2f wait %.2f]) other %.2f | gpu %.2f",
         n * 1000.0 / el, el / n, host / n, p.draw / n, p.assemble / n, p.bind / n, p.sig / n, p.texup / n,
         p.compile / n, p.clear / n, p.copy / n, p.sync / n, p.present / n, p.swap / n, p.wait / n, (el - host) / n,
         p.gpu / n);
    LOGR("perf: per frame %.0f draws (%.0f raw) %.0fk verts %.1f MB upload %.1f overflow | %.1f tex uploads %.2f MB, "
         "%.1f new tex, %.1f new targets, %.1f target uploads, %.1f shrinks, %.1f read-backs %.1f late | %llu shaders "
         "%llu psos this second",
         p.draws / n, p.raw / n, p.verts / n / 1000.0, p.upload / n / 1048576.0, p.overflow / n, p.texups / n,
         p.texup_bytes / n / 1048576.0, p.newtex / n, p.targets / n, p.rtups / n, p.shrinks / n, p.syncs / n,
         p.lates / n, (unsigned long long)p.shaders, (unsigned long long)p.psos);
    g_perf = Perf{};
    perf_start = PerfClock::now();
    perf_frames = 0;
}

void VulkanBackend::Present(const Surface& front) {
    PerfScope perf_present{g_perf.present};
    bool shown = false;
    if (cap_) {
        FinishCapture(front);
        shown = true;
    } else if ((g_capture.exchange(false) || CaptureDue()) && CaptureOn()) {
        StartCapture();
    }
    ResizeSwap();
    VTarget* t = EnsureTarget(front, false);
    if (t && LatchCpuWrite(*t)) UploadTarget(*t, front);
    if (ramp_dirty_) UploadGammaRamp();
    Transition(*lut_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    uint32_t bi = 0;
    bool acquired = false;
    if (swap_ && !swap_lost_) {
        Frame& f = CurrentFrame();
        VkResult r = vkAcquireNextImageKHR(dev_, swap_, UINT64_MAX, f.acquired, VK_NULL_HANDLE, &bi);
        acquired = r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR;
        if (r == VK_SUBOPTIMAL_KHR || r == VK_ERROR_OUT_OF_DATE_KHR) swap_lost_ = true;
        if (acquired) {
            pending_acquire_ = f.acquired;
            present_image_ = bi;
        }
    }
    if (acquired) {
        DrawFrame(t, bi);
        if (shown) CapShown(bi);
        if (g_screenshot.exchange(false)) Screenshot(bi);
    }
    FrameTimestamp(true);
    Submit(false, acquired);
    present_fence_[presents_ % (kFrames + 1)].store(timeline_value_);
    DeliverReports();
    auto perf_swap = PerfClock::now();
    if (acquired) PresentSwap(bi);
    g_perf.swap += PerfMs(perf_swap);

    presents_++;
    presents_seen_.store(presents_);
    prev_sided_.swap(sided_);
    sided_.clear();
    WriteFrameTime();
    frame_++;
    ReleaseOldTextures();
    ResetFrame();
    CountFps();
    if (diag::Perf()) LogPerf();
    ApplySettings();
}

void VulkanBackend::WaitGpuFrame(uint64_t n) {
    uint64_t fence = present_fence_[n % (kFrames + 1)].load();
    if (fence > Completed()) WaitTimeline(fence);
}

void VulkanBackend::ApplySettings() {
    if (texturedumper::Update()) ReleasePackTextures();
    float want = app::RenderScale(guest_w_, guest_h_);
    if (std::fabs(want - scale_) > 1e-4f) {
        Flush();
        for (auto& [key, tg] : targets_) {
            CurrentFrame().garbage.push_back(tg.img);
            if (tg.fb) CurrentFrame().garbage.push_back(tg.fb);
        }
        targets_.clear();
        scale_ = want;
        bound_rt_ = bound_ds_ = nullptr;
        want_ = active_ = {};
    }
    wide_ = app::WideFactor();
}

// Read-backs

VulkanBackend::CopySource VulkanBackend::PrepareCopySource(VTarget& t, ImageRef reuse) {
    CopySource out{t.img.get(), nullptr};
    if (t.w != t.gw || t.h != t.gh || t.fmt != kBgra) {
        out.down = Shrink(*t.img, t.gw, t.gh, {.into = reuse});
        out.img = out.down.get();
    }
    return out;
}

bool VulkanBackend::TargetStale(uint32_t data) const {
    auto it = target_used_.find(data);
    uint64_t used = it == target_used_.end() ? 0 : it->second;
    return frame_ - used > kStaleFrames;
}

void VulkanBackend::Sync(uint32_t data, uint32_t) {
    auto it = targets_.find(TargetMapKey(data, false));
    if (it == targets_.end() || !it->second.drawn || TargetStale(data)) return;
    PerfScope perf_sync{g_perf.sync};
    g_perf.syncs++;
    VTarget& t = it->second;
    CopySource src = PrepareCopySource(t, nullptr);
    if (!src.img) return;
    std::vector<uint8_t> px;
    if (!ReadBack(*src.img, t.gw, t.gh, px)) return;
    uint32_t pitch = GuestPitch(t.gw);
    uint8_t* guest = static_cast<uint8_t*>(kernel::HostPtr(data));
    for (uint32_t y = 0; y < t.gh; y++) std::memcpy(guest + size_t(y) * pitch, &px[size_t(y) * t.gw * 4], t.gw * 4);
    t.read = std::move(px);
    t.drawn = false;
    t.sig = MemSig(t.data, t.bytes);
}

void VulkanBackend::SyncLate(uint32_t data) {
    if (!DeliverLate(data)) Sync(data, 0);
    RecordLate(data);
}

bool VulkanBackend::DeliverLate(uint32_t data) {
    std::lock_guard lock(late_lock_);
    auto it = late_.find(data);
    if (it == late_.end()) return false;
    Late& l = it->second;
    uint64_t done = Completed();
    uint64_t now = presents_seen_.load();
    int best = -1;
    for (int i = 0; i < kFrames; i++) {
        bool finished = l.fence[i] && l.fence[i] <= done;
        bool recent = now - l.frame[i] <= uint64_t(kFrames) + 1;
        bool newest = best < 0 || l.fence[i] > l.fence[best];
        if (finished && recent && newest) best = i;
    }
    if (best < 0) return false;
    if (l.fence[best] > l.delivered) {
        vmaInvalidateAllocation(g_vma, l.rb[best]->mem, 0, VK_WHOLE_SIZE);
        const uint8_t* p = l.rb[best]->map;
        uint32_t pitch = GuestPitch(l.gw);
        uint8_t* dst = static_cast<uint8_t*>(kernel::HostPtr(data));
        for (uint32_t y = 0; y < l.gh; y++) std::memcpy(dst + size_t(y) * pitch, p + size_t(y) * l.gw * 4, l.gw * 4);
        l.delivered = l.fence[best];
    }
    return true;
}

void VulkanBackend::ResetLate(Late& l, const VTarget& t) {
    for (auto& r : l.rb)
        if (r) CurrentFrame().garbage_buffers.push_back(r);
    if (l.down) CurrentFrame().garbage.push_back(l.down);
    l = Late{};
    l.gw = t.gw;
    l.gh = t.gh;
}

void VulkanBackend::RecordLateCopy(Late& l, uint32_t slot, Image& src) {
    uint64_t bytes = uint64_t(l.gw) * l.gh * 4;
    if (!l.rb[slot]) {
        l.rb[slot] = MakeBuffer(bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);
        if (!l.rb[slot]) return;
    }
    Transition(src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkBufferImageCopy c{};
    c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    c.imageExtent = {l.gw, l.gh, 1};
    vkCmdCopyImageToBuffer(cb_, src.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, l.rb[slot]->buf, 1, &c);
    l.fence[slot] = timeline_value_ + 1;
    l.frame[slot] = presents_;
    l.next = (slot + 1) % kFrames;
}

void VulkanBackend::RecordLate(uint32_t data) {
    auto it = targets_.find(TargetMapKey(data, false));
    if (it == targets_.end() || TargetStale(data)) return;
    VTarget& t = it->second;
    PerfScope perf_sync{g_perf.sync};
    g_perf.lates++;
    t.sig = MemSig(t.data, t.bytes);
    t.read.clear();
    std::lock_guard lock(late_lock_);
    Late& l = late_[data];
    if (l.gw != t.gw || l.gh != t.gh) ResetLate(l, t);
    uint32_t slot = l.next;
    if (l.fence[slot] > Completed()) return;
    CopySource src = PrepareCopySource(t, l.down);
    if (!src.img) return;
    if (src.down) l.down = src.down;
    RecordLateCopy(l, slot, *src.img);
}

ImageRef VulkanBackend::Shrink(Image& src, uint32_t w, uint32_t h, const ShrinkOptions& o) {
    g_perf.shrinks++;
    ImageRef down = o.into;
    if (!down || down->w != w || down->h != h) {
        down = MakeImage(kBgra, w, h, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                          VK_IMAGE_USAGE_SAMPLED_BIT);
        if (!down) return nullptr;
    }
    Attach keep = want_;
    VTarget* keep_rt = bound_rt_;
    VTarget* keep_ds = bound_ds_;
    SetBlitTarget(*down);
    float uv[4] = {0, 0, 1, 1};
    if (src.depth) {
        // a depth target's picture: its depth as grey
        Transition(src, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    Blit(src, uv, {.alpha = true, .taps = o.taps, .mul = o.mul});
    Transition(*down, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    want_ = keep;
    bound_rt_ = keep_rt;
    bound_ds_ = keep_ds;
    return down;
}

bool VulkanBackend::ReadBack(Image& img, uint32_t w, uint32_t h, std::vector<uint8_t>& out) {
    BufferRef rb = MakeBuffer(uint64_t(w) * h * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);
    if (!rb) return false;
    Transition(img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkBufferImageCopy c{};
    c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    c.imageExtent = {w, h, 1};
    vkCmdCopyImageToBuffer(cb_, img.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rb->buf, 1, &c);
    Flush();
    vmaInvalidateAllocation(g_vma, rb->mem, 0, VK_WHOLE_SIZE);
    out.assign(rb->map, rb->map + size_t(w) * h * 4);
    return true;
}

constexpr int kBmpHeaderBytes = 54;

void WriteBmp(const std::filesystem::path& path, uint32_t w, uint32_t h, const std::vector<uint8_t>& px) {
    FILE* f = OpenFile(path, "wb");
    if (!f) return;
    uint8_t hd[kBmpHeaderBytes] = {'B', 'M'};
    auto put = [&](int o, uint32_t v) { std::memcpy(hd + o, &v, 4); };
    put(2, kBmpHeaderBytes + uint32_t(px.size()));
    put(10, kBmpHeaderBytes);
    put(14, 40);
    put(18, w);
    put(22, uint32_t(-int32_t(h)));
    hd[26] = 1;
    hd[28] = 32;
    put(34, uint32_t(px.size()));
    fwrite(hd, 1, kBmpHeaderBytes, f);
    fwrite(px.data(), 1, px.size(), f);
    fclose(f);
}

void VulkanBackend::CapSave(Image& img, uint32_t w, uint32_t h, float mul, const char* name) {
    // a depth image is sampled through its depth view: depth in red, as d3d12.cpp's kDepthRead view gives it
    ImageRef down = Shrink(img, w, h, {.mul = mul});
    std::vector<uint8_t> px;
    if (!down || !ReadBack(*down, w, h, px)) return;
    WriteBmp(cap_dir_ / (std::string(name) + ".bmp"), w, h, px);
}

void VulkanBackend::CapSave(VTarget& t, const char* name) { CapSave(*t.img, t.gw, t.gh, 1, name); }

void VulkanBackend::Screenshot(uint32_t bi) {
    std::vector<uint8_t> px;
    if (!ReadShown(bi, px)) return;
    std::error_code ec;
    std::filesystem::path dir = app::DataDir() / "screenshots";
    std::filesystem::create_directories(dir, ec);
    std::time_t now = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &now);
#else
    localtime_r(&now, &tm);
#endif
    char stamp[64];
    std::strftime(stamp, sizeof(stamp), "%Y%m%d_%H%M%S", &tm);
    std::filesystem::path path = dir / (platform::Utf8(app::kGame.name) + "_" + stamp + ".png");
    uint32_t w = bb_w_, h = bb_h_;
    std::thread([path, copy = std::move(px), w, h] {
        texturedumper::WriteBgraPng(path, w, h, copy);
        LOGI("screenshot: %s", path.string().c_str());
    }).detach();
}

void VulkanBackend::CapShown(uint32_t bi) {
    std::vector<uint8_t> px;
    if (!ReadShown(bi, px)) return;
    WriteBmp(shown_dir_ / "shown.bmp", bb_w_, bb_h_, px);
}

void VulkanBackend::CapStep(const Surface& rt) {
    uint32_t d = rt.header ? mem::VirtualFromPhysical(rt.data) : 0;
    if (d == cap_rt_) return;
    auto it = targets_.find(TargetMapKey(cap_rt_, false));
    if (cap_rt_ && it != targets_.end()) {
        char name[32];
        snprintf(name, sizeof(name), "%04u_%08X", cap_i_, cap_rt_);
        CapSave(it->second, name);
    }
    auto z = targets_.find(TargetMapKey(cap_rt_, true));
    if (cap_rt_ && z != targets_.end()) {
        char name[32];
        snprintf(name, sizeof(name), "%04u_%08X_z", cap_i_, cap_rt_);
        CapSave(z->second, name);
    }
    cap_rt_ = d;
}

void VulkanBackend::SetGamma(const uint8_t* ramp) {
    std::memcpy(ramp_, ramp, sizeof(ramp_));
    ramp_dirty_ = true;
}

}  // namespace

std::unique_ptr<Backend> CreateVulkanBackend() { return std::make_unique<VulkanBackend>(); }

}  // namespace gpu
#endif
