// Which renderer draws: Windows builds have Vulkan (XBE_VULKAN, the default) and D3D12, chosen by the renderer
// setting, D3D12 also when Vulkan fails to start; Linux has Vulkan alone
#include "app/app.h"
#include "core/log.h"
#include "gpu/backend.h"

namespace gpu {

namespace {
bool g_vulkan_chosen = false;
}

std::unique_ptr<Backend> CreateBackend() {
#if defined(_WIN32) && defined(XBE_VULKAN)
    g_vulkan_chosen = app::Get().renderer == app::kRendererVulkan;
    LOGR("Renderer: %s", g_vulkan_chosen ? "Vulkan" : "Direct3D 12");
    return g_vulkan_chosen ? CreateVulkanBackend() : CreateD3D12Backend();
#elif defined(_WIN32)
    return CreateD3D12Backend();
#elif defined(XBE_VULKAN)
    g_vulkan_chosen = true;
    return CreateVulkanBackend();
#else
    return nullptr;
#endif
}

std::unique_ptr<Backend> CreateFallbackBackend() {
#if defined(_WIN32) && defined(XBE_VULKAN)
    if (!g_vulkan_chosen) return nullptr;
    LOGW("Renderer: Vulkan did not start, drawing with Direct3D 12");
    g_vulkan_chosen = false;
    return CreateD3D12Backend();
#else
    return nullptr;
#endif
}

}  // namespace gpu
