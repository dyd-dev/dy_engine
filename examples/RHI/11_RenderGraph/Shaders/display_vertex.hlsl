struct Output { float4 position:SV_Position; float2 uv:TEXCOORD0; };
Output main(uint id:SV_VertexID) {
    const float2 positions[3]={float2(-1,-1),float2(3,-1),float2(-1,3)};
    Output result; result.position=float4(positions[id],0,1);
    result.uv=positions[id]*float2(0.5,-0.5)+0.5; return result;
}
