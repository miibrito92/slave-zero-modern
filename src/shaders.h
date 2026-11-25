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

// Post pass: scene -> swap chain, with gamma and optional sharpening.
static const char kPostHlsl[] = R"HLSL(
cbuffer Post : register(b0) {
    float  gInvGamma;
    float  gSharpen;
    float2 gTexel;
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

float4 ps_post(PO i) : SV_Target {
    float3 c = gScene.Sample(gLin, i.uv).rgb;
    if (gSharpen > 0.0) {
        float3 n = gScene.Sample(gLin, i.uv + float2(0.0, -gTexel.y)).rgb;
        float3 s = gScene.Sample(gLin, i.uv + float2(0.0,  gTexel.y)).rgb;
        float3 e = gScene.Sample(gLin, i.uv + float2( gTexel.x, 0.0)).rgb;
        float3 w = gScene.Sample(gLin, i.uv + float2(-gTexel.x, 0.0)).rgb;
        float3 mn = min(c, min(min(n, s), min(e, w)));
        float3 mx = max(c, max(max(n, s), max(e, w)));
        // contrast-adaptive weight: sharpen less where local contrast is already high
        float3 amp = saturate(min(mn, 1.0 - mx) / max(mx, 1e-4));
        float3 wgt = -sqrt(amp) * (gSharpen * 0.2);
        c = saturate((c + (n + s + e + w) * wgt) / (1.0 + 4.0 * wgt));
    }
    c = pow(saturate(c), gInvGamma);
    return float4(c, 1.0);
}
)HLSL";
