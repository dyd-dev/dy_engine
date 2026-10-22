Texture2D<float4> sourceImage:register(t0);
SamplerState sourceSampler:register(s1);
// 전체 화면 UV로 원본의 mip 0을 읽어 출력한다. 여기서는 RGB와 알파를 별도로 수정하지 않는다.
float4 main(float4 position:SV_Position,float2 uv:TEXCOORD0):SV_Target0
{
    return sourceImage.SampleLevel(sourceSampler,uv,0);
}
