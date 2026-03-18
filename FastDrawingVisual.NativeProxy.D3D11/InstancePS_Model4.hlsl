static const float kTypeFillRect = 0.0f;
static const float kTypeStrokeRect = 1.0f;
static const float kTypeFillEllipse = 2.0f;
static const float kTypeStrokeEllipse = 3.0f;
static const float kTypeLine = 4.0f;
static const float kAntiAliasWidth = 1.0f;

struct PSInput
{
    float4 pos : SV_Position;
    float2 local : TEXCOORD0;
    float2 halfSize : TEXCOORD1;
    float4 data0 : TEXCOORD2;
    float4 fillColor : TEXCOORD3;
    float4 strokeColor : TEXCOORD4;
    float4 misc : TEXCOORD5;
};

float CoverageFromSignedDistance(float signedDistance)
{
    return saturate(1.0f - smoothstep(-kAntiAliasWidth, kAntiAliasWidth, signedDistance));
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
    float outerCoverage = FillCoverageFromRoundedBox(p, halfSize, radius);
    float2 innerHalf = max(halfSize - strokeWidth, 0.0f);
    if (innerHalf.x <= 0.0f || innerHalf.y <= 0.0f)
    {
        return outerCoverage;
    }

    float innerRadius = max(radius - strokeWidth, 0.0f);
    float innerCoverage = FillCoverageFromRoundedBox(p, innerHalf, innerRadius);
    return saturate(outerCoverage - innerCoverage);
}

float FillCoverageFromEllipse(float2 p, float2 radius)
{
    float2 safeRadius = max(radius, 0.0001f);
    float metric = length(p / safeRadius);
    float aa = kAntiAliasWidth / max(min(safeRadius.x, safeRadius.y), 1.0f);
    return saturate(1.0f - smoothstep(1.0f - aa, 1.0f + aa, metric));
}

float StrokeCoverageFromEllipse(float2 p, float2 outerRadius, float strokeWidth)
{
    float outerCoverage = FillCoverageFromEllipse(p, outerRadius);
    float2 innerRadius = outerRadius - strokeWidth;
    if (innerRadius.x <= 0.0f || innerRadius.y <= 0.0f)
    {
        return outerCoverage;
    }

    float innerCoverage = FillCoverageFromEllipse(p, innerRadius);
    return saturate(outerCoverage - innerCoverage);
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

float4 main(PSInput input) : SV_Target
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

    color *= coverage;
    return color;
}
