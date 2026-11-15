#ifdef PROBE_VERTEX
float4 main(uint vertex:SV_VertexID):SV_Position
{
    float2 p=float2((vertex<<1)&2,vertex&2);
    return float4(p*2.0-1.0,0,1);
}
#else
Texture2D<uint2> ownerTexture:register(t0);
float4 main(float4 position:SV_Position):SV_Target0
{
    uint2 owner=ownerTexture.Load(int3(uint2(position.xy),0));
    return float4(float2(owner)/255.0,0,1);
}
#endif
