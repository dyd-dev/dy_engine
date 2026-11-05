StructuredBuffer<uint> words:register(t0);
RWTexture2D<float4> outputImage:register(u1);
struct Instance {column_major float4x4 model,previous;};
StructuredBuffer<Instance> instances:register(t2);
cbuffer Count:register(b15) {uint4 counts;}
[numthreads(64,1,1)]
void main(uint3 id:SV_DispatchThreadID) {
    if(id.x>=counts.x)return;
    uint value;
    if(counts.y==0)value=words[id.x];
    else {
        uint row=id.x/32,component=id.x%32,k=component%16;
        float4x4 m=component<16?instances[row].model:instances[row].previous;
        value=asuint(m[k%4][k/4]);
    }
    outputImage[uint2(id.x,0)]=float4(value&255u,(value>>8u)&255u,(value>>16u)&255u,value>>24u)/255.0;
}
