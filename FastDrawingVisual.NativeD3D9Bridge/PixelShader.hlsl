#ifndef PRIMITIVE_MODE
#define PRIMITIVE_MODE 0
#endif

float4 gColor : register(c0);
// x = halfStrokePx (0 for fill), y = aaWidthPx
float4 gParams : register(c1);
// rect: center.xy + halfSize.zw
// ellipse: center.xy + radius.zw
// line: p0.xy + p1.zw
float4 gData0 : register(c2);

float SdBox(float2 p, float2 center, float2 halfSize)
{
    float2 d = abs(p - center) - halfSize;
    float outside = length(max(d, 0.0));
    float inside = min(max(d.x, d.y), 0.0);
    return outside + inside;
}

float SdEllipseApprox(float2 p, float2 center, float2 radius)
{
    float2 safeRadius = max(radius, float2(0.0001, 0.0001));
    float2 q = (p - center) / safeRadius;
    return (length(q) - 1.0) * min(safeRadius.x, safeRadius.y);
}

float SdSegment(float2 p, float2 a, float2 b)
{
    float2 pa = p - a;
    float2 ba = b - a;
    float denom = max(dot(ba, ba), 0.0001);
    float h = saturate(dot(pa, ba) / denom);
    return length(pa - ba * h);
}

float Coverage(float signedDistance, float halfStrokePx, float aaWidthPx)
{
    float dist = (halfStrokePx > 0.0)
        ? abs(signedDistance) - halfStrokePx
        : signedDistance;

    float aa = max(aaWidthPx, 0.0001);
    return saturate(0.5 - dist / aa);
}

float4 main(float2 pixelPos : TEXCOORD0) : COLOR0
{
    float signedDistance = 0.0;

#if PRIMITIVE_MODE == 0
    signedDistance = SdBox(pixelPos, gData0.xy, gData0.zw);
#elif PRIMITIVE_MODE == 1
    signedDistance = SdEllipseApprox(pixelPos, gData0.xy, gData0.zw);
#else
    signedDistance = SdSegment(pixelPos, gData0.xy, gData0.zw);
#endif

    float coverage = Coverage(signedDistance, gParams.x, gParams.y);
    return float4(gColor.rgb, gColor.a * coverage);
}
