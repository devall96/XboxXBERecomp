// The present: the game's frame to the window through AMD FidelityFX Super Resolution 1 (third_party/fsr1, MIT) and
// colour grading. Compiled ahead of time by build.bat into present_shaders.h (fxc, shader model 5.0), so the runtime
// needs neither the headers nor a compile at start.
//   ps_easu   EASU, edge-adaptive upscaling of the shown view to the output's size (when rendered smaller)
//   ps_final  RCAS sharpening, the console's gamma ramp and colour grading, into the swap chain's buffer
#define A_GPU 1
#define A_HLSL 1
#include "../../../third_party/fsr1/ffx_a.h"

Texture2D img : register(t0);
Texture2D lut : register(t1);
SamplerState ls : register(s0);
SamplerState ps : register(s1);

cbuffer P : register(b1) {
    uint4 con0;    // EASU: FsrEasuCon; final: x RCAS FsrRcasCon
    uint4 con1;
    uint4 con2;
    uint4 con3;
    float4 opts;   // final: x sharpen, y gamma ramp, zw the viewport's top left in the target
    float4 grade0; // exposure (stops), contrast, saturation, 1 to grade
    float4 grade1; // temperature, tint (-1 to 1)
    float4 filter; // x colour vision filter: 0 none, 1 protanopia, 2 deuteranopia, 3 tritanopia; y strength;
                   // z 1 with a colour LUT, w its size
};
Texture3D clut : register(t2);  // colour_lut: a .cube file's table

float4 vs(uint id : SV_VertexID) : SV_Position {
    float2 t = float2((id << 1) & 2, id & 2);
    return float4(t * float2(2, -2) + float2(-1, 1), 0, 1);
}

#define FSR_EASU_F 1
AF4 FsrEasuRF(AF2 p) { return img.GatherRed(ls, p); }
AF4 FsrEasuGF(AF2 p) { return img.GatherGreen(ls, p); }
AF4 FsrEasuBF(AF2 p) { return img.GatherBlue(ls, p); }
#define FSR_RCAS_F 1
AF4 FsrRcasLoadF(ASU2 p) { return img.Load(int3(p, 0)); }
void FsrRcasInputF(inout AF1 r, inout AF1 g, inout AF1 b) {}
#include "../../../third_party/fsr1/ffx_fsr1.h"

float4 ps_easu(float4 pos : SV_Position) : SV_Target {
    AF3 c;
    FsrEasuF(c, AU2(pos.xy), con0, con1, con2, con3);
    return float4(c, 1);
}

float3 Grade(float3 c) {
    c *= exp2(grade0.x);
    // white balance: warmer raises red and lowers blue, tint trades green against magenta
    c *= float3(1 + 0.1 * grade1.x, 1 - 0.1 * grade1.y, 1 - 0.1 * grade1.x) + float3(0.05, 0, 0.05) * grade1.y;
    float luma = dot(c, float3(0.2126, 0.7152, 0.0722));
    c = lerp(luma.xxx, c, grade0.z);
    c = (c - 0.5) * grade0.y + 0.5;
    return saturate(c);
}

// colour vision: the picture as the given kind of colour blindness sees it (LMS cone space, Vienot, Brettel and
// Mollon's projections), and what that loses shifted into the colours that remain (Fidaner, Lin and Ozguven's
// daltonization)
float3 Daltonize(float3 c, int kind) {
    const float3x3 to_lms = float3x3(17.8824, 43.5161, 4.11935, 3.45565, 27.1554, 3.86714, 0.0299566, 0.184309, 1.46709);
    const float3x3 to_rgb = float3x3(0.0809444479, -0.130504409, 0.116721066, -0.0102485335, 0.0540193266,
                                     -0.113614708, -0.000365296938, -0.00412161469, 0.693511405);
    float3 lms = mul(to_lms, c);
    if (kind == 1) lms.x = 2.02344 * lms.y - 2.52581 * lms.z;
    else if (kind == 2) lms.y = 0.494207 * lms.x + 1.24827 * lms.z;
    else lms.z = -0.395913 * lms.x + 0.801109 * lms.y;
    float3 err = c - mul(to_rgb, lms);
    float3 shift = float3(0, 0.7 * err.r + err.g, 0.7 * err.r + err.b);
    return saturate(c + shift * filter.y);
}

float4 ps_final(float4 pos : SV_Position) : SV_Target {
    int2 p = int2(pos.xy - opts.zw);
    float3 c;
    if (opts.x > 0) FsrRcasF(c.r, c.g, c.b, AU2(p), con0);
    else c = img.Load(int3(p, 0)).rgb;
    if (opts.y > 0) c = float3(lut.Sample(ps, float2(c.r, 0.5)).r, lut.Sample(ps, float2(c.g, 0.5)).g, lut.Sample(ps, float2(c.b, 0.5)).b);
    if (grade0.w > 0) c = Grade(c);
    if (filter.z > 0) {
        // the table's cell centres: 0 and 1 land on its first and last entries
        float n = filter.w;
        c = clut.SampleLevel(ls, saturate(c) * ((n - 1) / n) + 0.5 / n, 0).rgb;
    }
    if (filter.x > 0) c = Daltonize(c, int(filter.x));
    return float4(c, 1);
}
