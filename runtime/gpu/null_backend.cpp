// A backend that draws nothing, for a Linux build without the D3D12 renderer's library (XBE_NO_RENDERER): the game
// runs, sounds and takes input behind a blank window. Fences and reports complete at once, surfaces stay in guest
// memory as the CPU wrote them.
#ifdef XBE_NO_RENDERER
#include "core/log.h"
#include "gpu/backend.h"

namespace gpu {

namespace {

class NullBackend final : public Backend {
public:
    bool Init(uint32_t width, uint32_t height) override {
        LOGW("gpu: built without a renderer, %ux%u frames are not drawn", width, height);
        return true;
    }
    void Clear(const State&, uint32_t, const uint8_t*, uint32_t, uint32_t, float, uint32_t) override {}
    void Draw(const State&, uint32_t, uint32_t, uint32_t, const uint8_t*) override {}
    void Copy(const Surface&, const Surface&, uint32_t, uint32_t, uint32_t, float, float) override {}
    void Present(const Surface&) override {}
    void Sync(uint32_t, uint32_t) override {}
    void SyncLate(uint32_t) override {}
    void Flush() override {}
    void SetGamma(const uint8_t*) override {}
};

}  // namespace

std::unique_ptr<Backend> CreateBackend() { return std::make_unique<NullBackend>(); }

}  // namespace gpu
#endif
