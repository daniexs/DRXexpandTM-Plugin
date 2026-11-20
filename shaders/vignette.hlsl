
cbuffer EffectParams : register(b0)
{
    float intensity;
    float strength;
    float aspectRatio;
    float blurAmount;
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
    float4 color = sceneTexture.Sample(linearSampler, input.uv);

    if (blurAmount > 0.001)
    {
        uint textureWidth;
        uint textureHeight;
        sceneTexture.GetDimensions(textureWidth, textureHeight);

        float2 pixel = float2(
            2.0 / max((float)textureWidth, 1.0),
            2.0 / max((float)textureHeight, 1.0)
        );

        // Subtle full-screen 3x3 blur with a radius of about two pixels.
        float2 offset = pixel * blurAmount;
        float4 blurred = color * 0.36;
        blurred += sceneTexture.Sample(linearSampler, input.uv + float2( offset.x, 0.0)) * 0.10;
        blurred += sceneTexture.Sample(linearSampler, input.uv + float2(-offset.x, 0.0)) * 0.10;
        blurred += sceneTexture.Sample(linearSampler, input.uv + float2(0.0,  offset.y)) * 0.10;
        blurred += sceneTexture.Sample(linearSampler, input.uv + float2(0.0, -offset.y)) * 0.10;
        blurred += sceneTexture.Sample(linearSampler, input.uv + float2( offset.x,  offset.y)) * 0.06;
        blurred += sceneTexture.Sample(linearSampler, input.uv + float2(-offset.x,  offset.y)) * 0.06;
        blurred += sceneTexture.Sample(linearSampler, input.uv + float2( offset.x, -offset.y)) * 0.06;
        blurred += sceneTexture.Sample(linearSampler, input.uv - offset) * 0.06;

        color = lerp(color, blurred, saturate(blurAmount));
    }

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
