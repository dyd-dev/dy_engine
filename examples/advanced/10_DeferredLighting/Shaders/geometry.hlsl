struct Params { float4 sizeTime; float4 settings; float4 jitter; float4 extra; };
struct Input { float4 position :SV_Position; float2 uv :TEXCOORD0; };
struct Output { float4 c0 : SV_Target0; float4 c1 : SV_Target1; float4 c2 : SV_Target2; };
cbuffer Config:register(b15) { Params p; };

// Analytic surfaces keep this lighting exercise independent of model assets.
struct Surface { float3 position; float3 normal; float3 albedo; float distance; float material; };
float3 eye() { return float3(0.0,-6.0,3.5); }
float3 cameraRay(float2 uv, float2 size) {
    float3 forward=normalize(float3(0.0,0.0,0.8)-eye());
    float3 right=normalize(cross(forward,float3(0.0,0.0,1.0)));
    float3 up=cross(right,forward);
    float2 q=(uv*2.0-1.0)*float2(size.x/size.y,-1.0);
    return normalize(forward+0.55*(q.x*right+q.y*up));
}
float sphereHit(float3 origin,float3 direction,float3 center,float radius) {
    float3 oc=origin-center; float b=dot(oc,direction);
    float h=b*b-dot(oc,oc)+radius*radius;
    return h>0.0 ? -b-sqrt(h) : -1.0;
}
Surface traceScene(float3 rd) {
    Surface s; s.distance=40.0; s.position=eye()+rd*40.0;
    s.normal=float3(0.0,0.0,1.0); s.albedo=float3(0.04,0.08,0.14); s.material=-1.0;
    if(rd.z<-0.001) {
        float t=-eye().z/rd.z;
        if(t>0.0 && t<24.0) {
            s.distance=t; s.position=eye()+rd*t;
            float checker=frac((floor(s.position.x)+floor(s.position.y))*0.5)*2.0;
            s.albedo=lerp((float3)(0.16),(float3)(0.32),checker); s.material=0.0;
        }
    }
    for(int i=0;i<3;i++) {
        float3 center=float3(float(i-1)*1.9,0.0,0.9);
        float t=sphereHit(eye(),rd,center,0.88);
        if(t>0.0 && t<s.distance) {
            s.distance=t; s.position=eye()+rd*t; s.normal=normalize(s.position-center);
            s.albedo=i==0 ? float3(0.9,0.15,0.06) : (i==1 ? float3(0.15,0.65,0.9) : float3(0.95,0.64,0.18));
            s.material=float(i+1);
        }
    }
    return s;
}

Output main(Input input) {
float2 uv=input.uv; Output o;

Surface s=traceScene(cameraRay(uv,p.sizeTime.xy));
o.c0=float4(s.position,s.material>=0.0 ? 1.0 : 0.0);
o.c1=float4(s.normal,clamp(p.settings.y,0.05,1.0));
o.c2=float4(s.albedo,s.material>0.0 ? p.settings.z : 0.0);

return o;
}
