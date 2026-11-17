#pragma once
// DXGI_FORMAT for the code that names host formats outside the renderer (vertex formats, texture packs). Windows and
// a renderer built on vkd3d have the real header; otherwise the formats used here, at DXGI's documented values.
#if defined(_WIN32)
#include <dxgiformat.h>
#elif __has_include(<vkd3d_dxgiformat.h>)
#include <vkd3d_dxgiformat.h>
#else
enum DXGI_FORMAT {
    DXGI_FORMAT_UNKNOWN = 0,
    DXGI_FORMAT_R32G32B32A32_FLOAT = 2,
    DXGI_FORMAT_R32G32B32_FLOAT = 6,
    DXGI_FORMAT_R16G16B16A16_SNORM = 13,
    DXGI_FORMAT_R16G16B16A16_SINT = 14,
    DXGI_FORMAT_R32G32_FLOAT = 16,
    DXGI_FORMAT_R8G8B8A8_UNORM = 28,
    DXGI_FORMAT_R16G16_SNORM = 37,
    DXGI_FORMAT_R16G16_SINT = 38,
    DXGI_FORMAT_R32_FLOAT = 41,
    DXGI_FORMAT_R32_UINT = 42,
    DXGI_FORMAT_R8G8_UNORM = 49,
    DXGI_FORMAT_R16_SNORM = 58,
    DXGI_FORMAT_R16_SINT = 59,
    DXGI_FORMAT_R8_UNORM = 61,
    DXGI_FORMAT_BC1_UNORM = 71,
    DXGI_FORMAT_BC2_UNORM = 74,
    DXGI_FORMAT_BC3_UNORM = 77,
    DXGI_FORMAT_B8G8R8A8_UNORM = 87,
    DXGI_FORMAT_BC7_UNORM = 98,
};
#endif
