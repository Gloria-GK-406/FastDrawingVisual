struct PSInput
{
    float4 color : COLOR0;
};

float4 main(PSInput input) : COLOR0
{
    return input.color;
}
