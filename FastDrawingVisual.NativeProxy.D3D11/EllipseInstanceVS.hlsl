cbuffer ViewConstants : register(b0)
{
    float2 viewportSize;
    float2 padding;
};

struct VSInput
{
    float2 unitDir : POSITION;
    float blend : TEXCOORD2;
    float4 ellipse : TEXCOORD0;
    float thickness : TEXCOORD1;
    float4 color : COLOR0;
};

struct PSInput
{
    float4 pos : SV_Position;
    float2 local : TEXCOORD0;
    float2 radius : TEXCOORD1;
    float thickness : TEXCOORD2;
    float4 color : COLOR0;
};

PSInput main(VSInput input)
{
    PSInput output;

    float2 radius = float2(input.ellipse.z, input.ellipse.w);
    float2 outerRadius = radius;
    float2 innerRadius = float2(0.0f, 0.0f);
    if (input.thickness > 0.0f)
    {
        outerRadius += input.thickness * 0.5f;
        innerRadius = max(radius - input.thickness * 0.5f, 0.0f);
    }

    float2 scaledRadius = lerp(innerRadius, outerRadius, input.blend);
    float2 local = input.unitDir * scaledRadius;
    float2 pixelPos = float2(input.ellipse.x, input.ellipse.y) + local;
    float clipX = (pixelPos.x / viewportSize.x) * 2.0f - 1.0f;
    float clipY = 1.0f - (pixelPos.y / viewportSize.y) * 2.0f;

    output.pos = float4(clipX, clipY, 0.0f, 1.0f);
    output.local = local;
    output.radius = radius;
    output.thickness = input.thickness;
    output.color = input.color;
    return output;
}
