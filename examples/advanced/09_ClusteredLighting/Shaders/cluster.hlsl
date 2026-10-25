struct Params {
    float2 screenSize;
    float time;
    uint lightCount;
    float exposure;
};
cbuffer Config:register(b15) { Params p; };
RWStructuredBuffer<uint> indices:register(u0);

[numthreads(64,1,1)] void main(uint3 id:SV_DispatchThreadID) {
    uint cluster=id.x;
    uint x=cluster%16u,y=(cluster/16u)%16u,z=cluster/256u;
    float3 lower=float3(-6.0,-6.0,0.0)+float3(float(x),float(y),float(z))*float3(0.75,0.75,0.5);
    float3 upper=lower+float3(0.75,0.75,0.5);uint count=0u,base=cluster*65u;
    for(int i=0;i<int(p.lightCount);i++) {
        float f=float(i); float3 position=float3(sin(f*2.17+p.time*0.17)*4.0,cos(f*1.37+p.time*0.11)*3.0,0.5+frac(f*0.319)*2.5),closest=clamp(position,lower,upper);
        if(dot(position-closest,position-closest)<=2.4*2.4) indices[base+1u+count++]=uint(i);
    }
    indices[base]=count;
}
