float4 main(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target0
{
    return float4(uv, 0.0f, 1.0f);
}
