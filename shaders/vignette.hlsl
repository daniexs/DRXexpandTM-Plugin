
cbuffer EffectParams : register(b0)
{
    float intensity;
    float strength;
    float aspectRatio;
    float blurAmount;
    float shakeAmount;
    float shakePhase;
    float showerValue;
    float timeSeconds;
};

Texture2D sceneTexture : register(t0);
SamplerState linearSampler : register(s0);

struct VSOutput
{
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
};

float Hash21(float2 p)
{
    return frac(sin(dot(p, float2(127.1, 311.7))) * 43758.5453);
}

float Noise21(float2 p)
{
    float2 cell = floor(p);
    float2 local = frac(p);
    local = local * local * (3.0 - 2.0 * local);

    float a = Hash21(cell);
    float b = Hash21(cell + float2(1.0, 0.0));
    float c = Hash21(cell + float2(0.0, 1.0));
    float d = Hash21(cell + float2(1.0, 1.0));

    return lerp(lerp(a, b, local.x), lerp(c, d, local.x), local.y);
}

float Ellipse(float2 point, float2 radii)
{
    float distance = length(point / radii);
    return 1.0 - smoothstep(0.72, 1.12, distance);
}

void FlyShape(float2 uv, float2 position, float2 resolution, float seed,
              out float body, out float wings)
{
    float2 pixelDelta = (uv - position) * resolution;
    body = Ellipse(pixelDelta, float2(1.6, 1.1));

    float flutter = 0.5 + 0.5 * sin(timeSeconds * 145.0 + seed);
    float wingWidth = 2.1;
    float wingHeight = 0.9 + flutter * 0.35;
    float leftWing = Ellipse(pixelDelta - float2(-1.8, -0.8), float2(wingWidth, wingHeight));
    float rightWing = Ellipse(pixelDelta - float2(1.8, -0.8), float2(wingWidth, wingHeight));
    wings = max(leftWing, rightWing) * (0.12 + 0.22 * flutter);
}

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
    uint textureWidth;
    uint textureHeight;
    sceneTexture.GetDimensions(textureWidth, textureHeight);

    float2 shakeOffset = float2(
        sin(shakePhase * 75.3982),
        cos(shakePhase * 75.3982 * 0.83)
    ) * shakeAmount * float2(
        2.0 / max((float)textureWidth, 1.0),
        2.0 / max((float)textureHeight, 1.0)
    );

    float2 sceneUV = input.uv + shakeOffset;
    float4 color = sceneTexture.Sample(linearSampler, sceneUV);

    if (blurAmount > 0.001)
    {
        float2 pixel = float2(
            2.0 / max((float)textureWidth, 1.0),
            2.0 / max((float)textureHeight, 1.0)
        );

        // Subtle full-screen 3x3 blur with a radius of about two pixels.
        float2 offset = pixel * blurAmount;
        float4 blurred = color * 0.36;
        blurred += sceneTexture.Sample(linearSampler, sceneUV + float2( offset.x, 0.0)) * 0.10;
        blurred += sceneTexture.Sample(linearSampler, sceneUV + float2(-offset.x, 0.0)) * 0.10;
        blurred += sceneTexture.Sample(linearSampler, sceneUV + float2(0.0,  offset.y)) * 0.10;
        blurred += sceneTexture.Sample(linearSampler, sceneUV + float2(0.0, -offset.y)) * 0.10;
        blurred += sceneTexture.Sample(linearSampler, sceneUV + float2( offset.x,  offset.y)) * 0.06;
        blurred += sceneTexture.Sample(linearSampler, sceneUV + float2(-offset.x,  offset.y)) * 0.06;
        blurred += sceneTexture.Sample(linearSampler, sceneUV + float2( offset.x, -offset.y)) * 0.06;
        blurred += sceneTexture.Sample(linearSampler, sceneUV - offset) * 0.06;

        color = lerp(color, blurred, saturate(blurAmount));
    }

    // Shower-driven dullness and sweaty edge grime. The central 70% stays clear.
    if (showerValue <= 20.0)
    {
        float t = saturate((20.0 - showerValue) / 20.0);
        float luminance = dot(color.rgb, float3(0.2126, 0.7152, 0.0722));
        color.rgb = lerp(color.rgb, luminance.xxx, 0.30 * t);
        color.rgb = lerp(color.rgb, color.rgb * float3(1.035, 0.97, 0.88), 0.18 * t);

        float2 centerDistance = abs(input.uv - float2(0.5, 0.5));
        float edgeOutside = max(centerDistance.x - 0.35, centerDistance.y - 0.35);
        float edgeBand = smoothstep(0.0, 0.035, edgeOutside) *
                         (1.0 - smoothstep(0.12, 0.18, edgeOutside));
        float grimeNoise = Noise21(input.uv * float2(27.0, 19.0) + timeSeconds * 0.012);
        float blotches = smoothstep(0.42, 0.82, grimeNoise);
        float grime = edgeBand * (0.035 + 0.16 * blotches) * t;
        color.rgb = lerp(color.rgb, float3(0.19, 0.15, 0.105), grime);
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

    if (showerValue < 4.0)
    {
        float flyAlpha = saturate((4.0 - showerValue) / 4.0);
        float2 resolution = float2((float)textureWidth, (float)textureHeight);
        float time = timeSeconds;

        float2 fly1 = float2(
            0.5 + 0.43 * sin(time * 0.63 + 0.28 * sin(time * 1.7)),
            0.5 + 0.39 * cos(time * 0.82 + 0.24 * sin(time * 1.23)));
        float2 fly2 = float2(
            0.5 + 0.40 * sin(time * 0.51 + 2.1 + 0.22 * cos(time * 1.51)),
            0.5 + 0.43 * cos(time * 0.73 + 1.6 + 0.19 * sin(time * 1.31)));
        float2 fly3 = float2(
            0.5 + 0.44 * sin(time * 0.71 + 4.0 + 0.20 * sin(time * 1.87)),
            0.5 + 0.36 * cos(time * 0.59 + 3.2 + 0.23 * cos(time * 1.42)));

        float body1, wings1, body2, wings2, body3, wings3;
        FlyShape(input.uv, fly1, resolution, 0.4, body1, wings1);
        FlyShape(input.uv, fly2, resolution, 2.1, body2, wings2);
        FlyShape(input.uv, fly3, resolution, 4.0, body3, wings3);

        float bodyMask = saturate(body1 + body2 + body3) * flyAlpha;
        float wingMask = saturate(wings1 + wings2 + wings3) * flyAlpha;
        color.rgb = lerp(color.rgb, float3(0.055, 0.045, 0.035), bodyMask);
        color.rgb = lerp(color.rgb, float3(0.30, 0.27, 0.23), wingMask);
    }

    return color;
}
