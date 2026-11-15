//  SPDX-License-Identifier: MIT
//
//  ES-DE Frontend
//  ShadersD3D12.h
//
//  HLSL versions of the GLSL shaders in resources/shaders/glsl, used by RendererD3D12.
//  The shaders are compiled at runtime using D3DCompile().
//
//  Notes on the conversion from GLSL:
//  - The vertex color is an unsigned int in 0xRRGGBBAA format. It's read as R8G8B8A8_UNORM
//    which gives (A, B, G, R) on little-endian, so it's swizzled with .wzyx, matching the
//    "color.abgr = colorVertex.rgba" statement in the GLSL code.
//  - glm matrices are column-major which is also the default packing for HLSL constants,
//    so mul(MVPMatrix, position) gives the same result as in GLSL.
//  - Depth is not used, z is always set to 0.
//

#ifndef ES_CORE_RENDERER_SHADERS_D3D12_H
#define ES_CORE_RENDERER_SHADERS_D3D12_H

namespace ShadersD3D12
{
    // clang-format off
    static const char* const sSource = R"HLSL(

cbuffer DrawConstants : register(b0)
{
    float4x4 MVPMatrix;
    float2 texSize;
    float2 padding0;
    float4 clipRegion;
    float brightness;
    float saturation;
    float opacity;
    float dimming;
    float cornerRadius;
    float reflectionsFalloff;
    float blurStrength;
    uint shaderFlags;
};

Texture2D textureSampler0 : register(t0);
SamplerState sampler0 : register(s0);

struct VSInput {
    float2 position : POSITION;
    float2 texCoord : TEXCOORD0;
    float4 color : COLOR0;
};

struct PSInputCore {
    float4 svPosition : SV_POSITION;
    float2 position : TEXCOORD0;
    float2 texCoord : TEXCOORD1;
    float4 color : COLOR0;
};

struct PSInputBlur {
    float4 svPosition : SV_POSITION;
    float2 texCoord : TEXCOORD0;
};

struct PSInputScanlines {
    float4 svPosition : SV_POSITION;
    float2 texCoord : TEXCOORD0;
    float2 onex : TEXCOORD1;
    float2 oney : TEXCOORD2;
    float4 colorShift : COLOR0;
};

// shaderFlags:
// 0x00000001 - Premultiplied alpha (BGRA)
// 0x00000002 - Font texture
// 0x00000004 - Post processing
// 0x00000008 - Clipping
// 0x00000010 - Screen rotated 90 or 270 degrees
// 0x00000020 - Rounded corners
// 0x00000040 - Rounded corners with no anti-aliasing
// 0x00000080 - Convert pixel format

float4 toClipSpace(float2 position)
{
    float4 result = mul(MVPMatrix, float4(position, 0.0, 1.0));
    result.z = 0.0;
    return result;
}

// ---------------------------------------------------------------------------------------
// Core shader.
// ---------------------------------------------------------------------------------------

PSInputCore VSCore(VSInput input)
{
    PSInputCore output;
    output.svPosition = toClipSpace(input.position);
    output.position = input.position;
    output.texCoord = input.texCoord;
    output.color = input.color.wzyx;
    return output;
}

float4 PSCore(PSInputCore input) : SV_TARGET
{
    float2 position = input.position;
    float4 color = input.color;

    // Discard any pixels outside the clipping region.
    if (0 != (shaderFlags & 0x8u)) {
        if (position.x < clipRegion.x || position.y < clipRegion.y ||
            position.x > clipRegion.z || position.y > clipRegion.w)
            discard;
    }

    float4 sampledColor = textureSampler0.Sample(sampler0, input.texCoord);

    // Pixel format conversion.
    if (0 != (shaderFlags & 0x80u))
        sampledColor = sampledColor.bgra;

    // Rounded corners.
    if (0 != (shaderFlags & 0x20u) || 0 != (shaderFlags & 0x40u)) {
        float cornerRadiusClamped = cornerRadius;
        // Don't go beyond half the width and height.
        if (cornerRadiusClamped > texSize.x / 2.0)
            cornerRadiusClamped = texSize.x / 2.0;
        if (cornerRadiusClamped > texSize.y / 2.0)
            cornerRadiusClamped = texSize.y / 2.0;

        float2 center = position - texSize / 2.0;
        float2 q = abs(center) - (float2(texSize.x / 2.0, texSize.y / 2.0) - cornerRadiusClamped);
        float pixelDistance = length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - cornerRadiusClamped;

        if (pixelDistance > 0.0) {
            discard;
        }
        else {
            float pixelValue;
            if (0 != (shaderFlags & 0x20u))
                pixelValue = 1.0 - smoothstep(-0.75, 0.5, pixelDistance);
            else
                pixelValue = 1.0;

            if (0 != (shaderFlags & 0x01u))
                sampledColor *= pixelValue;
            else
                sampledColor.a *= pixelValue;
        }
    }

    // Brightness.
    if (brightness != 0.0) {
        sampledColor.rgb /= sampledColor.a;
        sampledColor.rgb += 0.3 * brightness;
        sampledColor.rgb *= sampledColor.a;
    }

    // Saturation, except for font textures.
    if (saturation != 1.0 && 0 == (shaderFlags & 0x2u)) {
        float3 grayscale;
        if (0 != (shaderFlags & 0x01u))
            grayscale = dot(sampledColor.bgr, float3(0.114, 0.587, 0.299)).xxx;
        else
            grayscale = dot(sampledColor.rgb, float3(0.299, 0.587, 0.114)).xxx;
        float3 blendedColor = lerp(grayscale, sampledColor.rgb, saturation);
        sampledColor = float4(blendedColor, sampledColor.a);
    }

    // For fonts the alpha information is stored in the red channel.
    if (0 != (shaderFlags & 0x2u))
        sampledColor = float4(1.0, 1.0, 1.0, sampledColor.r);

    // Premultiplied versus straight alpha.
    if (0 != (shaderFlags & 0x01u)) {
        sampledColor.rgb *= color.rgb;
        sampledColor *= color.a;
    }
    else {
        sampledColor *= color;
    }

    // Saturation for font textures.
    if (saturation != 1.0 && 0 != (shaderFlags & 0x2u)) {
        float3 grayscale = dot(sampledColor.rgb, float3(0.299, 0.587, 0.114)).xxx;
        float3 blendedColor = lerp(grayscale, sampledColor.rgb, saturation);
        sampledColor = float4(blendedColor, sampledColor.a);
    }

    // When post-processing the alpha channel is dropped.
    if (0 != (shaderFlags & 0x4u))
        sampledColor.a = 1.0;

    // Opacity.
    if (opacity != 1.0) {
        if (0 == (shaderFlags & 0x01u))
            sampledColor.a *= opacity;
        else
            sampledColor *= opacity;
    }

    // Dimming.
    if (dimming != 1.0)
        sampledColor *= float4(dimming, dimming, dimming, 1.0);

    // Reflections falloff.
    if (reflectionsFalloff > 0.0)
        sampledColor *= lerp(0.0, 1.0, reflectionsFalloff - position.y) / reflectionsFalloff;

    return sampledColor;
}

// ---------------------------------------------------------------------------------------
// Gaussian blur shaders.
// ---------------------------------------------------------------------------------------

PSInputBlur VSBlur(VSInput input)
{
    PSInputBlur output;
    output.svPosition = toClipSpace(input.position);
    output.texCoord = input.texCoord;
    return output;
}

float4 blur(float2 texCoord, float hstep, float vstep)
{
    float4 color = float4(0.0, 0.0, 0.0, 0.0);
    float2 tc;

    if (0 != (shaderFlags & 0x10u))
        tc = texCoord.yx;
    else
        tc = texCoord.xy;

    // 9-tap filter.
    color += textureSampler0.Sample(sampler0, float2(tc.x - 4.0 * blurStrength * hstep, tc.y - 4.0 * blurStrength * vstep)) * 0.0162162162;
    color += textureSampler0.Sample(sampler0, float2(tc.x - 3.0 * blurStrength * hstep, tc.y - 3.0 * blurStrength * vstep)) * 0.0540540541;
    color += textureSampler0.Sample(sampler0, float2(tc.x - 2.0 * blurStrength * hstep, tc.y - 2.0 * blurStrength * vstep)) * 0.1216216216;
    color += textureSampler0.Sample(sampler0, float2(tc.x - 1.0 * blurStrength * hstep, tc.y - 1.0 * blurStrength * vstep)) * 0.1945945946;
    color += textureSampler0.Sample(sampler0, float2(tc.x, tc.y)) * 0.2270270270;
    color += textureSampler0.Sample(sampler0, float2(tc.x + 1.0 * blurStrength * hstep, tc.y + 1.0 * blurStrength * vstep)) * 0.1945945946;
    color += textureSampler0.Sample(sampler0, float2(tc.x + 2.0 * blurStrength * hstep, tc.y + 2.0 * blurStrength * vstep)) * 0.1216216216;
    color += textureSampler0.Sample(sampler0, float2(tc.x + 3.0 * blurStrength * hstep, tc.y + 3.0 * blurStrength * vstep)) * 0.0540540541;
    color += textureSampler0.Sample(sampler0, float2(tc.x + 4.0 * blurStrength * hstep, tc.y + 4.0 * blurStrength * vstep)) * 0.0162162162;

    return float4(color.rgb, 1.0);
}

float4 PSBlurHorizontal(PSInputBlur input) : SV_TARGET
{
    return blur(input.texCoord, 1.0, 0.0);
}

float4 PSBlurVertical(PSInputBlur input) : SV_TARGET
{
    return blur(input.texCoord, 0.0, 1.0);
}

// ---------------------------------------------------------------------------------------
// Scanlines shader.
// ---------------------------------------------------------------------------------------

#define SPOT_WIDTH 0.9
#define SPOT_HEIGHT 0.75
#define COLOR_BOOST 1.45
#define InputGamma 2.4
#define OutputGamma 2.2

float4 gammaIn(float2 coords)
{
    return pow(max(textureSampler0.Sample(sampler0, coords), 0.0), InputGamma);
}

float weight(float w)
{
    if (w > 1.0)
        w = 1.0;
    w = 1.0 - w * w;
    return w * w;
}

PSInputScanlines VSScanlines(VSInput input)
{
    PSInputScanlines output;
    output.svPosition = toClipSpace(input.position);
    output.texCoord = input.texCoord;
    float4 sourceSize = float4(texSize, 1.0 / texSize);
    output.onex = float2(sourceSize.z, 0.0);
    output.oney = float2(0.0, sourceSize.w);
    output.colorShift = input.color.wzyx;
    return output;
}

float4 PSScanlines(PSInputScanlines input) : SV_TARGET
{
    bool rotated = (0 != (shaderFlags & 0x10u));

    float4 sourceSize;
    if (rotated)
        sourceSize = float4(texSize.yx, 1.0 / texSize.yx);
    else
        sourceSize = float4(texSize.xy, 1.0 / texSize.xy);

    float2 coords = input.texCoord * sourceSize.xy;
    float2 pixelCenter = floor(coords) + float2(0.5, 0.5);
    float2 textureCoords = pixelCenter * sourceSize.zw;

    float4 color = gammaIn(textureCoords);
    float dx = coords.x - pixelCenter.x;

    float hWeight00 = weight(dx / SPOT_WIDTH);
    color *= hWeight00;

    // Closest horizontal neighbour to blend.
    float2 coords01;
    if (dx > 0.0) {
        coords01 = input.onex;
        dx = 1.0 - dx;
    }
    else {
        coords01 = -input.onex;
        dx = 1.0 + dx;
    }
    float4 colorNB = gammaIn(textureCoords + coords01);
    float hWeight01 = weight(dx / SPOT_WIDTH);
    color = color + colorNB * hWeight01;

    // Vertical blending.
    float dy;
    if (rotated)
        dy = coords.x - pixelCenter.x;
    else
        dy = coords.y - pixelCenter.y;
    float vWeight00 = weight(dy / SPOT_HEIGHT);
    color *= vWeight00;

    // Closest vertical neighbour to blend.
    float2 coords10;
    if (dy > 0.0) {
        coords10 = input.oney;
        dy = 1.0 - dy;
    }
    else {
        coords10 = -input.oney;
        dy = 1.0 + dy;
    }
    colorNB = gammaIn(textureCoords + coords10);
    float vWeight10 = weight(dy / SPOT_HEIGHT);

    color = color + colorNB * (vWeight10 * hWeight00);
    colorNB = gammaIn(textureCoords + coords01 + coords10);
    color = color + colorNB * (vWeight10 * hWeight01);
    color *= COLOR_BOOST;

    float4 colorTemp = saturate(pow(max(color, 0.0), 1.0 / OutputGamma));

    // Brightness.
    if (brightness != 0.0) {
        colorTemp.rgb /= colorTemp.a;
        colorTemp.rgb += 0.3 * brightness;
        colorTemp.rgb *= colorTemp.a;
    }

    // Saturation.
    if (saturation != 1.0) {
        float3 grayscale = dot(colorTemp.bgr, float3(0.114, 0.587, 0.299)).xxx;
        float3 blendedColor = lerp(grayscale, colorTemp.rgb, saturation);
        colorTemp = float4(blendedColor, colorTemp.a);
    }

    // Color shift.
    colorTemp.rgb *= input.colorShift.rgb;
    colorTemp.a *= input.colorShift.a;

    return float4(colorTemp.rgb, colorTemp.a * opacity);
}

)HLSL";
    // clang-format on

} // namespace ShadersD3D12

#endif // ES_CORE_RENDERER_SHADERS_D3D12_H
