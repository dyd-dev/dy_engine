struct Vertex { float4 position; float4 color; };
RWStructuredBuffer<Vertex> outputVertices:register(u0);
cbuffer Parameters:register(b15) { float4 parameters; };
[numthreads(3,1,1)]
void main(uint3 threadID:SV_DispatchThreadID) {
    uint id=threadID.x;
    const float2 positions[3]={float2(-0.7,-0.6),float2(0.7,-0.6),float2(0,0.7)};
    const float3 colors[3]={float3(1,0.2,0.15),float3(0.2,1,0.3),float3(0.2,0.4,1)};
    float angle=parameters.x*0.4,c=cos(angle),s=sin(angle);
    float2 p=positions[id];
    outputVertices[id].position=float4(c*p.x-s*p.y,s*p.x+c*p.y,0,1);
    outputVertices[id].color=float4(colors[id]*(0.75+0.25*sin(parameters.x+float(id))),1);
}
