
cbuffer EffectParams : register(b0)
{
    float intensity;
    float strength;
    float aspectRatio;
    float padding;
};

Texture2D sceneTexture : register(t0);
SamplerState linearSampler : register(s0);

struct VSOutput
{
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
};

// Fullscreen triangle tanpa vertex buffer.
VSOutput VSMain(uint vertexID : SV_VertexID)
{
    VSOutput output;

    float2 uv = float2(
        (vertexID << 1) & 2,
        vertexID & 2
    );

    output.position = float4(
        uv.x * 2.0 - 1.0,
        1.0 - uv.y * 2.0,
        0.0,
        1.0
    );

    output.uv = uv;

    return output;
}

float4 PSMain(VSOutput input) : SV_TARGET
{
    float4 color = sceneTexture.Sample(
        linearSampler,
        input.uv
    );

    float2 centered = input.uv - float2(0.5, 0.5);

    centered.x *= aspectRatio;

    float distanceFromCenter = length(centered);

    float vignette = smoothstep(
        0.35,
        0.95,
        distanceFromCenter
    );

    // Kekuatan maksimum penggelapan 30%.
    float darkening = vignette
                    * saturate(intensity)
                    * strength;

    color.rgb *= (1.0 - darkening);

    return color;
}
