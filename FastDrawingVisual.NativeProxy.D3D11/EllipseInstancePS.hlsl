struct PSInput
{
    float4 pos : SV_Position;
    float2 local : TEXCOORD0;
    float2 radius : TEXCOORD1;
    float thickness : TEXCOORD2;
    float4 color : COLOR0;
};

float4 main(PSInput input) : SV_Target
{
    return input.color;
}
