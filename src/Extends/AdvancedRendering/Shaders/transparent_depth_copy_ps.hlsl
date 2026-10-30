Texture2D<float> sourceDepth:register(t5);
float main(float4 position:SV_Position):SV_Target0
{
    return sourceDepth.Load(int3(int2(position.xy),0));
}
