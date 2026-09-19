struct Input
{
    float2 position : TEXCOORD0;
    float4 instanceTransform : TEXCOORD1;
    float4 instanceColor : TEXCOORD2;
};
struct Output { float4 position : SV_POSITION; float4 color : TEXCOORD0; };
Output main(Input input)
{
    Output output;
    output.position = float4(input.position * input.instanceTransform.zw + input.instanceTransform.xy, 0, 1);
    output.color = input.instanceColor;
    return output;
}
