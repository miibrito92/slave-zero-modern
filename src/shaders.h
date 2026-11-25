#pragma once

// Scene shader: one uber-shader covering every primitive type the engine emits.
// Vertex flags (nointerpolation):
//   bits 0-1  colour mode: 0 vertex colour, 1 texture, 2 texture*vertex, 3 texture*lightmap
//   bits 2-3  alpha source: 0 one, 1 constant (vertex alpha), 2 texture, 3 texture*constant
//   bit  4    cutout: discard where texture alpha < 0.5
//   bit  5    alpha test: discard where final alpha < AlphaRef
static const char kSceneHlsl[] = R"HLSL(
cbuffer Frame : register(b0) {
    float2 gAxis;    // virtual pixel -> NDC scale
    float2 gCenter;  // virtual screen centre
    float  gZn;
    float  gBiasStep;
    float  gLightmapScale;
    float  gAlphaRef;
};
Texture2D    gTex0  : register(t0);
Texture2D    gTex1  : register(t1);
SamplerState gSamp0 : register(s0);
SamplerState gSamp1 : register(s1);

struct VSIn {
    float4 pos   : POSITION;   // virtual-screen x, y, 1/z, decal bias level
    float4 col   : COLOR;
    float4 uv    : TEXCOORD0;  // base uv, lightmap uv
    uint   flags : TEXCOORD1;
};
struct VSOut {
    float4 pos : SV_Position;
    float4 col : COLOR;
    float4 uv  : TEXCOORD0;
    nointerpolation uint flags : TEXCOORD1;
};

VSOut vs_main(VSIn i) {
    VSOut o;
    float w = 1.0 / max(i.pos.z, 1e-12);
    float2 ndc = float2((i.pos.x - gCenter.x) * gAxis.x, (gCenter.y - i.pos.y) * gAxis.y);
    o.pos = float4(ndc * w, gZn * (1.0 + i.pos.w * gBiasStep), w);
    o.col = i.col;
    o.uv = i.uv;
    o.flags = i.flags;
    return o;
}

float4 ps_main(VSOut i) : SV_Target {
    uint mode = i.flags & 3u;
    uint asrc = (i.flags >> 2) & 3u;
    float4 t  = gTex0.Sample(gSamp0, i.uv.xy);
    float3 lm = gTex1.Sample(gSamp1, i.uv.zw).rgb;
    float3 rgb = i.col.rgb;
    if (mode == 1u) rgb = t.rgb;
    else if (mode == 2u) rgb = t.rgb * i.col.rgb;
    else if (mode == 3u) rgb = t.rgb * lm * gLightmapScale;
    float a = 1.0;
    if (asrc == 1u) a = i.col.a;
    else if (asrc == 2u) a = t.a;
    else if (asrc == 3u) a = t.a * i.col.a;
    if ((i.flags & 16u) != 0u && t.a < 0.5) discard;
    if ((i.flags & 32u) != 0u && a < gAlphaRef) discard;
    return float4(saturate(rgb), a);
}
)HLSL";

// Post pass: scene -> swap chain, with gamma, sharpening and optional screen filters.
// gMode: 0 none, 1 scanlines, 2 CRT, 3 CRT curved, 4 retro 640x480 pixels, 5 VHS
static const char kPostHlsl[] = R"HLSL(
cbuffer Post : register(b0) {
    float  gInvGamma;
    float  gSharpen;
    float2 gTexel;     // 1 / scene size
    float2 gOutSize;   // output viewport size in pixels
    float2 gOutOrigin; // output viewport top-left in pixels
    float  gMode;
    float  gStrength;
    float  gLines;     // emulated scanline count
    float  gTime;
    float2 gVirtual;   // virtual (engine) resolution covered by the scene, e.g. 853x480
    float2 gPad;
};
Texture2D    gScene : register(t0);
SamplerState gLin   : register(s0);

struct PO { float4 pos : SV_Position; float2 uv : TEXCOORD0; };

PO vs_post(uint id : SV_VertexID) {
    PO o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.uv = uv;
    o.pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return o;
}

float Hash(float2 p) { return frac(sin(dot(p, float2(12.9898, 78.233))) * 43758.5453); }

float3 Sharpened(float2 uv) {
    float3 c = gScene.Sample(gLin, uv).rgb;
    if (gSharpen <= 0.0) return c;
    float3 n = gScene.Sample(gLin, uv + float2(0.0, -gTexel.y)).rgb;
    float3 s = gScene.Sample(gLin, uv + float2(0.0,  gTexel.y)).rgb;
    float3 e = gScene.Sample(gLin, uv + float2( gTexel.x, 0.0)).rgb;
    float3 w = gScene.Sample(gLin, uv + float2(-gTexel.x, 0.0)).rgb;
    float3 mn = min(c, min(min(n, s), min(e, w)));
    float3 mx = max(c, max(max(n, s), max(e, w)));
    float3 amp = saturate(min(mn, 1.0 - mx) / max(mx, 1e-4));
    float3 wgt = -sqrt(amp) * (gSharpen * 0.2);
    return saturate((c + (n + s + e + w) * wgt) / (1.0 + 4.0 * wgt));
}

float2 Warp(float2 uv) {
    float2 c = uv * 2.0 - 1.0;
    float k = 0.10 * gStrength;
    c *= 1.0 + k * float2(c.y * c.y * 0.6, c.x * c.x);
    return c * 0.5 + 0.5;
}

float4 ps_post(PO i) : SV_Target {
    int mode = (int)(gMode + 0.5);
    float s = gStrength;
    float2 uv = i.uv;
    float2 px = i.pos.xy - gOutOrigin;  // output pixel inside the viewport

    if (mode == 3) {
        uv = Warp(uv);
        if (any(uv < 0.0) || any(uv > 1.0)) return float4(0, 0, 0, 1);
    }

    float3 c;
    if (mode == 4) {  // the engine's original pixel grid, shown sharp
        float2 snapped = (floor(uv * gVirtual) + 0.5) / gVirtual;
        c = gScene.Sample(gLin, lerp(uv, snapped, s)).rgb;
    } else if (mode == 5) {  // VHS
        float row = floor(uv.y * gLines);
        float wobble = (sin(uv.y * 40.0 + gTime * 3.0) * 0.0006 + (Hash(float2(row, floor(gTime * 24.0))) - 0.5) * 0.0012) * s;
        float2 u = uv + float2(wobble, 0.0);
        float bleed = 0.0025 * s;
        c.r = gScene.Sample(gLin, u + float2(bleed, 0.0)).r;
        c.g = gScene.Sample(gLin, u).g;
        c.b = gScene.Sample(gLin, u - float2(bleed, 0.0)).b;
        float3 blur = (gScene.Sample(gLin, u + float2(gTexel.x * 3.0, 0.0)).rgb +
                       gScene.Sample(gLin, u - float2(gTexel.x * 3.0, 0.0)).rgb) * 0.5;
        c = lerp(c, blur, 0.35 * s);
    } else {
        c = Sharpened(uv);
    }

    c = pow(saturate(c), gInvGamma);

    if (mode == 1 || mode == 2 || mode == 3) {
        // soft glow from a few wide taps (CRT modes)
        if (mode != 1) {
            float2 r = gTexel * 3.0;
            float3 glow = gScene.Sample(gLin, uv + float2(r.x, r.y)).rgb + gScene.Sample(gLin, uv + float2(-r.x, r.y)).rgb +
                          gScene.Sample(gLin, uv + float2(r.x, -r.y)).rgb + gScene.Sample(gLin, uv + float2(-r.x, -r.y)).rgb;
            glow = pow(saturate(glow * 0.25), gInvGamma);
            c += glow * glow * 0.25 * s;
        }
        // scanlines: gaussian beam that widens on bright pixels
        float y = uv.y * gLines;
        float f = frac(y) - 0.5;
        float lum = dot(c, float3(0.299, 0.587, 0.114));
        float width = lerp(0.20, 0.42, saturate(lum));
        float beam = exp(-(f * f) / (2.0 * width * width));
        float norm = 1.0 / (width * 2.5066);  // keep average brightness
        c *= lerp(1.0, beam * min(norm, 1.8), s);
        // aperture-grille mask (CRT modes)
        if (mode != 1) {
            int m = (int)fmod(floor(px.x), 3.0);
            float lo = 1.0 - 0.45 * s;
            float3 mask = m == 0 ? float3(1.0, lo, lo) : m == 1 ? float3(lo, 1.0, lo) : float3(lo, lo, 1.0);
            c *= mask * (1.0 + 0.30 * s);
        }
    }
    if (mode == 3) {  // vignette + rounded corners
        float2 d = abs(uv * 2.0 - 1.0);
        float corner = 1.0 - smoothstep(0.92, 1.0, max(d.x * 0.98 + d.y * d.y * 0.03, d.y * 0.98 + d.x * d.x * 0.03));
        c *= corner * (1.0 - 0.35 * s * dot(d, d) * 0.5);
    }
    if (mode == 5) {
        float n = Hash(px + frac(gTime) * 917.0) - 0.5;
        c += n * 0.06 * s;
        c *= 1.0 - 0.25 * s * dot(uv * 2.0 - 1.0, uv * 2.0 - 1.0) * 0.5;
    }
    return float4(saturate(c), 1.0);
}
)HLSL";

// Fallback post pass (gamma only), used if the filter shader fails to compile.
static const char kPostFallbackHlsl[] = R"HLSL(
cbuffer Post : register(b0) { float gInvGamma; float3 gPad; };
Texture2D gScene : register(t0);
SamplerState gLin : register(s0);
struct PO { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
PO vs_post(uint id : SV_VertexID) {
    PO o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.uv = uv;
    o.pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return o;
}
float4 ps_post(PO i) : SV_Target { return float4(pow(saturate(gScene.Sample(gLin, i.uv).rgb), gInvGamma), 1.0); }
)HLSL";
