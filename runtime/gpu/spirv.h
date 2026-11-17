#pragma once
// HLSL to SPIR-V for the Vulkan renderer, through glslang: the same translated shaders the D3D12 renderer compiles
// with D3DCompile. Registers map to the renderer's push descriptor bindings: b0 and b1 to 0 and 1, t0..t3 to 2..5,
// s0..s3 to 6..9. Vertex inputs take locations in declaration order, which is register order (shaders.cpp).
#include <cstdint>
#include <string>
#include <vector>

namespace gpu::spirv {

constexpr uint32_t kBindingUbo = 0, kBindingTexture = 2, kBindingSampler = 6, kBindings = 10;

// profile as D3DCompile takes it: vs_5_0, ps_5_0 or gs_5_0; false with the compiler's message in error
bool Compile(const std::string& hlsl, const char* entry, const char* profile, std::vector<uint32_t>& out,
             std::string& error);

}  // namespace gpu::spirv
