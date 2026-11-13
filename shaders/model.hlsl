// MMD-style model shader for ukagaka3d (Direct3D 11, compiled with fxc /Zpr).
// Output is premultiplied alpha so it can be composited onto the desktop.

cbuffer SceneCB : register(b0)
{
    float4x4 gViewProj;
    float4x4 gView;
    float3   gLightDir;    // direction the light travels (MMD default: (-0.5, -1.0, 0.5))
    float    gEdgePixel;   // world-space size of one pixel at distance 1
    float3   gLightColor;  // MMD default: 154/255
    float    gPad0;
    float3   gEyePos;
    float    gPad1;
};

static const uint FLAG_TEXTURE    = 1;
static const uint FLAG_SPHERE     = 2;
static const uint FLAG_SPHERE_ADD = 4;
static const uint FLAG_TOON       = 8;

cbuffer MaterialCB : register(b1)
{
    float4 gDiffuse;
    float3 gSpecular;
    float  gShininess;
    float3 gAmbient;
    uint   gFlags;
    float4 gEdgeColor;
    float  gEdgeSize;
    float3 gPad2;
    float4 gTexMul;
    float4 gTexAdd;
    float4 gSphereMul;
    float4 gSphereAdd;
    float4 gToonMul;
    float4 gToonAdd;
};

Texture2D    gTexture : register(t0);
Texture2D    gSphere  : register(t1);
Texture2D    gToon    : register(t2);
SamplerState gWrap    : register(s0);
SamplerState gClamp   : register(s1);

struct VSInput
{
    float3 position  : POSITION;
    float3 normal    : NORMAL;
    float2 uv        : TEXCOORD0;
    float  edgeScale : TEXCOORD1;
};

struct PSInput
{
    float4 position : SV_Position;
    float3 normal   : NORMAL;
    float2 uv       : TEXCOORD0;
    float3 toEye    : TEXCOORD1;
    float2 sphereUv : TEXCOORD2;
};

PSInput VSMain(VSInput input)
{
    PSInput output;
    output.position = mul(float4(input.position, 1.0), gViewProj);
    output.normal = input.normal;
    output.uv = input.uv;
    output.toEye = gEyePos - input.position;
    float3 viewNormal = normalize(mul(float4(input.normal, 0.0), gView).xyz);
    output.sphereUv = viewNormal.xy * float2(0.5, -0.5) + 0.5;
    return output;
}

float4 PSMain(PSInput input) : SV_Target
{
    float3 n = normalize(input.normal);
    float3 l = -gLightDir;

    float4 color = float4(saturate(gDiffuse.rgb * gLightColor + gAmbient), gDiffuse.a);

    if (gFlags & FLAG_TEXTURE)
    {
        float4 tex = gTexture.Sample(gWrap, input.uv);
        tex = tex * gTexMul + gTexAdd;
        color *= tex;
    }
    if (gFlags & FLAG_SPHERE)
    {
        float3 sph = gSphere.Sample(gWrap, input.sphereUv).rgb * gSphereMul.rgb + gSphereAdd.rgb;
        if (gFlags & FLAG_SPHERE_ADD)
            color.rgb += sph;
        else
            color.rgb *= sph;
    }
    if (gFlags & FLAG_TOON)
    {
        float lightNormal = dot(n, l);
        float3 toon = gToon.Sample(gClamp, float2(0.0, 0.5 - lightNormal * 0.5)).rgb;
        color.rgb *= toon * gToonMul.rgb + gToonAdd.rgb;
    }
    if (gShininess > 0.0)
    {
        float3 h = normalize(l + normalize(input.toEye));
        color.rgb += gSpecular * gLightColor * pow(max(0.0, dot(h, n)), gShininess);
    }

    color = saturate(color);
    if (color.a < 1.0 / 255.0)
        discard;
    return float4(color.rgb * color.a, color.a);
}

// ---------------------------------------------------------------------------
// Edge (outline) pass: inverted hull, front faces culled, roughly constant
// thickness in pixels.
// ---------------------------------------------------------------------------
struct EdgeOutput
{
    float4 position : SV_Position;
};

EdgeOutput VSEdge(VSInput input)
{
    EdgeOutput output;
    float distanceToEye = length(gEyePos - input.position);
    float thickness = gEdgeSize * input.edgeScale * gEdgePixel * distanceToEye;
    float3 position = input.position + normalize(input.normal) * thickness;
    output.position = mul(float4(position, 1.0), gViewProj);
    return output;
}

float4 PSEdge(EdgeOutput input) : SV_Target
{
    float a = gEdgeColor.a;
    if (a < 1.0 / 255.0)
        discard;
    return float4(gEdgeColor.rgb * a, a);
}
