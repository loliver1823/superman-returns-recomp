// Xenos kPointList (VGT_DRAW_INITIATOR prim_type == 1) expansion — iter-370.
//
// WHY A GEOMETRY SHADER. A point sprite's quad corners are offsets in SCREEN
// space, so they can only be applied AFTER the vertex shader has transformed
// the point. Our vertex decode happens on the CPU, which only ever sees the
// PRE-transform attributes — offsetting there would displace the sprite in
// object space, which is not the same thing at any non-identity transform.
// The console's setup unit does this expansion post-VS; a GS is the D3D12
// stage that sits in the same place.
//
// WHY ONE SHADER COVERS EVERY POINT DRAW. Every XenosRecomp-translated vertex
// shader emits the SAME output signature — SV_Position plus the fixed
// INTERPOLATORS list in tools/XenosRecomp-clone/XenosRecomp/shader_recompiler.cpp
// (TEXCOORD0..15, COLOR0, COLOR1, NORMAL0). So this single pass-through GS
// links against all of them. If that list ever changes, this file and the
// struct below must change with it.
//
// Build (dxc from the same bin that produced the cached VS/PS DXIL, so the
// validator/signing version matches):
//   tools/XenosRecomp-clone/thirdparty/dxc-bin/bin/x64/dxc.exe -T gs_6_0 -E main \
//     -Fo data/shader_cache/point_sprite.gs.dxil tools/xenosrecomp/point_sprite.gs.hlsl
// Regenerate with tools/build_point_gs.ps1.

// The shared per-draw constant block (b2, space4 — root parameter 2, visible to
// all stages). c19.xy is written by the host as the point's NDC radius; see
// D3D12Backend::tryXlatDraw for the PA_SU_POINT_SIZE conversion.
cbuffer SharedConstants : register(b2, space4)
{
    float4 g_Shared[32];
};

struct Vtx
{
    float4 Position  : SV_Position;
    float4 TexCoord0  : TEXCOORD0;
    float4 TexCoord1  : TEXCOORD1;
    float4 TexCoord2  : TEXCOORD2;
    float4 TexCoord3  : TEXCOORD3;
    float4 TexCoord4  : TEXCOORD4;
    float4 TexCoord5  : TEXCOORD5;
    float4 TexCoord6  : TEXCOORD6;
    float4 TexCoord7  : TEXCOORD7;
    float4 TexCoord8  : TEXCOORD8;
    float4 TexCoord9  : TEXCOORD9;
    float4 TexCoord10 : TEXCOORD10;
    float4 TexCoord11 : TEXCOORD11;
    float4 TexCoord12 : TEXCOORD12;
    float4 TexCoord13 : TEXCOORD13;
    float4 TexCoord14 : TEXCOORD14;
    float4 TexCoord15 : TEXCOORD15;
    float4 Color0     : COLOR0;
    float4 Color1     : COLOR1;
    float4 Normal0    : NORMAL0;
};

[maxvertexcount(4)]
void main(point Vtx input[1], inout TriangleStream<Vtx> stream)
{
    Vtx v = input[0];

    // The VS epilogue leaves oPos in CLIP space in both viewport modes (VTE-on
    // passes through; VTE-off remaps pixels via g_NdcScaleOffset). An NDC offset
    // therefore has to be scaled by w to survive the perspective divide.
    const float2 ndcRadius = g_Shared[19].xy;
    const float2 offset = ndcRadius * v.Position.w;
    const float2 center = v.Position.xy;

    // Triangle-strip corners. Winding is irrelevant here — the translated
    // pipeline rasterizes with CullMode NONE, matching the console's default for
    // these sprites.
    const float2 kCorner[4] =
    {
        float2(-1.0, -1.0), float2(1.0, -1.0),
        float2(-1.0,  1.0), float2(1.0,  1.0),
    };

    [unroll]
    for (int i = 0; i < 4; ++i)
    {
        Vtx o = v;
        o.Position.xy = center + kCorner[i] * offset;
        stream.Append(o);
    }
}
