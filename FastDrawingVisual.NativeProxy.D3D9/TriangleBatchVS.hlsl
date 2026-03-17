struct VSInput
{
    float3 pos : POSITION0;
    float4 color : COLOR0;
};

struct PSInput
{
    float4 pos : POSITION;
    float4 color : COLOR0;
};

PSInput main(VSInput input)
{
    PSInput output;
    output.pos = float4(input.pos, 1.0f);
    output.color = input.color;
    return output;
}
