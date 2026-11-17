#ifdef XBE_VULKAN
#include "gpu/spirv.h"

#include <glslang/Public/ResourceLimits.h>
#include <glslang/Public/ShaderLang.h>
// an installed glslang keeps SPIRV/ under glslang/, its source tree (a FetchContent build) beside it
#if __has_include(<glslang/SPIRV/GlslangToSpv.h>)
#include <glslang/SPIRV/GlslangToSpv.h>
#else
#include <SPIRV/GlslangToSpv.h>
#endif

#include <cstring>
#include <mutex>

namespace gpu::spirv {

namespace {

EShLanguage Stage(const char* profile) {
    if (!std::strncmp(profile, "ps", 2)) return EShLangFragment;
    if (!std::strncmp(profile, "gs", 2)) return EShLangGeometry;
    return EShLangVertex;
}

void Init() {
    static std::once_flag once;
    std::call_once(once, [] { glslang::InitializeProcess(); });
}

}  // namespace

bool Compile(const std::string& hlsl, const char* entry, const char* profile, std::vector<uint32_t>& out,
             std::string& error) {
    Init();
    EShLanguage stage = Stage(profile);
    glslang::TShader shader(stage);
    const char* text = hlsl.c_str();
    int length = int(hlsl.size());
    shader.setStringsWithLengths(&text, &length, 1);
    shader.setEntryPoint(entry);
    shader.setSourceEntryPoint(entry);
    shader.setEnvInput(glslang::EShSourceHlsl, stage, glslang::EShClientVulkan, 100);
    shader.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_3);
    shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_6);
    shader.setEnvTargetHlslFunctionality1();
    shader.setHlslIoMapping(true);
    shader.setAutoMapLocations(true);
    shader.setShiftBinding(glslang::EResUbo, kBindingUbo);
    shader.setShiftBinding(glslang::EResTexture, kBindingTexture);
    shader.setShiftBinding(glslang::EResSampler, kBindingSampler);
    shader.setTextureSamplerTransformMode(EShTexSampTransKeep);
    auto messages = EShMessages(EShMsgSpvRules | EShMsgVulkanRules | EShMsgReadHlsl | EShMsgHlslOffsets);
    if (!shader.parse(GetDefaultResources(), 100, false, messages)) {
        error = shader.getInfoLog();
        return false;
    }
    glslang::TProgram program;
    program.addShader(&shader);
    if (!program.link(messages) || !program.mapIO()) {
        error = program.getInfoLog();
        return false;
    }
    glslang::SpvOptions options;
    options.disableOptimizer = false;  // HLSL needs SPIRV-Tools' legalization (textures passed to functions)
    spv::SpvBuildLogger logger;
    out.clear();
    glslang::GlslangToSpv(*program.getIntermediate(stage), out, &logger, &options);
    std::string messages_text = logger.getAllMessages();
    if (out.empty()) {
        error = messages_text.empty() ? "no SPIR-V" : messages_text;
        return false;
    }
    return true;
}

}  // namespace gpu::spirv
#endif
