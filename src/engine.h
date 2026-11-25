// Engine-side data layouts for the Slave Zero hwAPI renderer interface.
// Every offset here was recovered from the shipped 3dfx.dll / d3d.dll; see docs/hwapi.md.
#pragma once
#include <stdint.h>

template <class T> static inline T& F(const void* base, uint32_t off) {
    return *(T*)((uint8_t*)base + off);
}

// ---- render context (rc), owned by SlaveZero.exe ----
namespace rc {
constexpr uint32_t Persp = 0x08;        // -> perspective struct
constexpr uint32_t Priv = 0x0c;         // renderer-private pointer (ours)
constexpr uint32_t OffX = 0x20;         // float viewport offset
constexpr uint32_t OffY = 0x24;
constexpr uint32_t ShadowMode = 0x58;   // int: lightmap debug mode (0 = normal)
constexpr uint32_t Gamma = 0x64;        // float
constexpr uint32_t ReadFb = 0x6c;       // void*: ReadFrameBuffer result
constexpr uint32_t Opt = 0x70;          // -> options struct
constexpr uint32_t StrBuf = 0x84;       // char[256]
constexpr uint32_t CbAlloc = 0x214;
constexpr uint32_t CbFree = 0x21c;
constexpr uint32_t CbError = 0x22c;
constexpr uint32_t CbLog = 0x234;
constexpr uint32_t CbAnimTexture = 0x268;
}  // namespace rc

namespace persp {
constexpr uint32_t Width = 0x00;   // int
constexpr uint32_t Height = 0x04;  // int
constexpr uint32_t WidthF = 0x08;
constexpr uint32_t HeightF = 0x0c;
constexpr uint32_t ScaleX = 0x20;
constexpr uint32_t ScaleY = 0x24;
constexpr uint32_t CenterX = 0x2c;
constexpr uint32_t CenterY = 0x30;
}  // namespace persp

namespace opt {
constexpr uint32_t ModeWidth = 0x1030;
constexpr uint32_t ModeHeight = 0x1034;
constexpr uint32_t Lightmaps = 0x1154;
constexpr uint32_t VSync = 0x1160;
constexpr uint32_t Fog = 0x11b0;
}  // namespace opt

// ---- primitive (convex polygon) handed to ProcessBatchOfPrimatives ----
namespace prim {
constexpr uint32_t FanStart = 0x04;  // u16
constexpr uint32_t Count = 0x06;     // u16
constexpr uint32_t Flags = 0x08;     // u32, see Pf*
constexpr uint32_t Flags2 = 0x0c;    // u32, see P2*
constexpr uint32_t Tex = 0x18;       // -> texture
constexpr uint32_t Lightmap = 0x1c;  // -> lightmap ("shadow map")
constexpr uint32_t LmDynamic = 0x22; // s16: nonzero = use LmDynData this frame
constexpr uint32_t FrameIdx = 0x24;  // -> u16 animation frame index
constexpr uint32_t LmDynData = 0x28; // -> 16-bit pixels
constexpr uint32_t Verts = 0x30;     // -> array of vertex pointers
}  // namespace prim

constexpr uint32_t PfVertexColor = 0x80000000;  // untextured, gouraud
constexpr uint32_t PfTexture = 0x40000000;      // texture only (fullbright)
constexpr uint32_t PfTextureLit = 0x20000000;   // texture * vertex colour
constexpr uint32_t PfLightmapped = 0x10000000;  // texture * lightmap
constexpr uint32_t PfConstAlpha = 0x00800000;   // low byte = alpha
constexpr uint32_t PfTexAlphaA = 0x00400000;
constexpr uint32_t PfTexAlphaB = 0x00200000;
constexpr uint32_t PfTranslucent = 0x00e00000;
constexpr uint32_t P2Decal = 0x00020000;
constexpr uint32_t P2SortEarly = 0x00000400;

// ---- vertex record (camera space) ----
namespace vtx {
constexpr uint32_t X = 0x14, Y = 0x18, Z = 0x1c;
constexpr uint32_t U = 0x20, V = 0x24, LU = 0x28, LV = 0x2c;
constexpr uint32_t R = 0x30, G = 0x34, B = 0x38;
constexpr uint32_t Oow = 0x4c;
}  // namespace vtx

// ---- texture ----
namespace tex {
constexpr uint32_t Width = 0x24, Height = 0x28, Flags = 0x2c;
constexpr uint32_t Data = 0x30, Pls = 0x34, Frames = 0x3c;  // Frames is u16
}  // namespace tex
constexpr uint32_t TfAlpha = 0x1, TfAlpha2 = 0x4, TfMips = 0x8, Tf1555 = 0x80, Tf4444 = 0x100, Tf8888 = 0x200;

// ---- lightmap ----
namespace lm {
constexpr uint32_t Data = 0x00, Pls = 0x24, Dims = 0x2c /* u8: log2 w <<4 | log2 h */, Flags = 0x2d;
}
constexpr uint8_t LfDynamicShown = 0x08;

constexpr uint32_t kPlsStride = 0x48;  // bytes per PLS record; we report this from SizeofPLS

// ---- display-mode record filled by EnumerateDisplayModes ----
namespace mode {
constexpr uint32_t Renderer = 0x000, Menu = 0x100, Device = 0x200;
constexpr uint32_t Index = 0x300, Width = 0x304, Height = 0x308, Bpp = 0x30c, Size = 0x310;
}  // namespace mode

typedef void*(__cdecl* AllocFn)(uint32_t);
typedef void(__cdecl* FreeFn)(void*);
typedef void(__cdecl* ErrorFn)(const char*, int);
typedef void(__cdecl* LogFn)(const char*, ...);
typedef void(__cdecl* AnimFn)(void* frameIdx, uint32_t frameCount);
