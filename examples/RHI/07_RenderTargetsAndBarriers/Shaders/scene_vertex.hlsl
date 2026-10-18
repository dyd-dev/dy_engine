struct Output { float4 position:SV_Position; float4 color:COLOR0; };
Output main(uint id:SV_VertexID) {
    const float2 positions[3] = {float2(-0.7,-0.6),float2(0.7,-0.6),float2(0,0.7)};
    const float4 colors[3] = {float4(1,0.2,0.15,1),float4(0.2,1,0.3,1),float4(0.2,0.4,1,1)};
    Output result; result.position=float4(positions[id],0,1); result.color=colors[id]; return result;
}
