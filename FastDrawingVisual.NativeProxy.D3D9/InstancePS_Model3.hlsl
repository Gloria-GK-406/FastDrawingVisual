static const float kAntiAliasScale = 0.75f;
static const float kMinAntiAliasWidth = 0.35f;
static const float kMaxAntiAliasWidth = 1.25f;

struct PSInput
{
    float2 local : TEXCOORD0;
    float2 halfSize : TEXCOORD1;
    float4 data0 : TEXCOORD2;
    float4 fillColor : TEXCOORD3;
    float4 strokeColor : TEXCOORD4;
    float4 misc : TEXCOORD5;
};

float AntiAliasWidth(float signedDistance)
{
    float width = (abs(ddx(signedDistance)) + abs(ddy(signedDistance))) * kAntiAliasScale;
    return clamp(width, kMinAntiAliasWidth, kMaxAntiAliasWidth);
}

float CoverageFromSignedDistance(float signedDistance)
{
    float aa = AntiAliasWidth(signedDistance);
    return saturate(0.5f - (signedDistance / aa));
}

float StrokeCoverageFromSignedDistance(float signedDistance, float strokeWidth)
{
    float halfStrokeWidth = strokeWidth * 0.5f;
    float bandDistance = abs(signedDistance + halfStrokeWidth) - halfStrokeWidth;
    return CoverageFromSignedDistance(bandDistance);
}

float SdRoundedBox(float2 p, float2 halfSize, float radius)
{
    float cornerRadius = min(radius, min(halfSize.x, halfSize.y));
    float2 innerHalf = max(halfSize - cornerRadius, 0.0f);
    float2 q = abs(p) - innerHalf;
    return length(max(q, 0.0f)) + min(max(q.x, q.y), 0.0f) - cornerRadius;
}

float FillCoverageFromRoundedBox(float2 p, float2 halfSize, float radius)
{
    return CoverageFromSignedDistance(SdRoundedBox(p, halfSize, radius));
}

float StrokeCoverageFromRoundedBox(float2 p, float2 halfSize, float strokeWidth, float radius)
{
    if (strokeWidth <= 0.0f)
    {
        return 0.0f;
    }

    return StrokeCoverageFromSignedDistance(SdRoundedBox(p, halfSize, radius), strokeWidth);
}

float SdEllipse(float2 p, float2 radius)
{
    float2 safeRadius = max(radius, 0.0001f);
    float2 q = p / safeRadius;
    float metric = length(q);
    float2 gradient = q / safeRadius;
    float gradientLength = max(length(gradient), 0.0001f);
    return (metric - 1.0f) / gradientLength;
}

float FillCoverageFromEllipse(float2 p, float2 radius)
{
    return CoverageFromSignedDistance(SdEllipse(p, radius));
}

float StrokeCoverageFromEllipse(float2 p, float2 outerRadius, float strokeWidth)
{
    if (strokeWidth <= 0.0f)
    {
        return 0.0f;
    }

    return StrokeCoverageFromSignedDistance(SdEllipse(p, outerRadius), strokeWidth);
}

float SdLineSegment(float2 p, float2 a, float2 b)
{
    float2 ba = b - a;
    float denom = max(dot(ba, ba), 0.0001f);
    float h = saturate(dot(p - a, ba) / denom);
    return length((p - a) - ba * h);
}

float LineCoverage(float2 p, float2 a, float2 b, float strokeWidth)
{
    float signedDistance = SdLineSegment(p, a, b) - strokeWidth * 0.5f;
    return CoverageFromSignedDistance(signedDistance);
}

float4 main(PSInput input) : COLOR0
{
    float type = input.misc.z;
    float strokeWidth = input.misc.x;
    float radius = input.misc.y;
    float coverage = 0.0f;
    float4 color = 0.0f;

    if (type < 0.5f)
    {
        coverage = FillCoverageFromRoundedBox(input.local, input.halfSize, radius);
        color = input.fillColor;
    }
    else if (type < 1.5f)
    {
        coverage = StrokeCoverageFromRoundedBox(input.local, input.halfSize, strokeWidth, radius);
        color = input.strokeColor;
    }
    else if (type < 2.5f)
    {
        coverage = FillCoverageFromEllipse(input.local, input.data0.xy);
        color = input.fillColor;
    }
    else if (type < 3.5f)
    {
        coverage = StrokeCoverageFromEllipse(input.local, input.data0.xy, strokeWidth);
        color = input.strokeColor;
    }
    else
    {
        coverage = LineCoverage(input.local, input.data0.xy, input.data0.zw, strokeWidth);
        color = input.strokeColor;
    }

    return color * coverage;
}
