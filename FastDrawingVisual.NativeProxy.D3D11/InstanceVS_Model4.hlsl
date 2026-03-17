cbuffer ViewConstants : register(b0)
{
    float2 viewportSize;
    float2 padding;
};

struct VSInput
{
    float2 quad : POSITION;
    float4 bounds : TEXCOORD0;
    float4 data0 : TEXCOORD1;
    float4 fillColor : TEXCOORD2;
    float4 strokeColor : TEXCOORD3;
    float4 misc : TEXCOORD4;
};

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

PSInput main(VSInput input)
{
    PSInput output;

    float2 halfSize = input.bounds.zw * 0.5f;
    float2 center = input.bounds.xy + halfSize;
    float2 pixelPos = center + input.quad * halfSize;

    float clipX = (pixelPos.x / viewportSize.x) * 2.0f - 1.0f;
    float clipY = 1.0f - (pixelPos.y / viewportSize.y) * 2.0f;

    output.pos = float4(clipX, clipY, 0.0f, 1.0f);
    output.local = input.quad * halfSize;
    output.halfSize = halfSize;
    output.data0 = input.data0;
    output.fillColor = input.fillColor;
    output.strokeColor = input.strokeColor;
    output.misc = input.misc;
    return output;
}
