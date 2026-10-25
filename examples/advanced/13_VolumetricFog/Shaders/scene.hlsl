struct Params {
    float2 screenSize;
    float time;
    float exposure;
    float density;
    float heightFalloff;
    uint stepCount;
};
struct Input { float4 position :SV_Position; float2 uv :TEXCOORD0; };
struct Output { float4 c0 : SV_Target0; };
cbuffer Config:register(b15) { Params p; };

// Analytic surfaces keep this lighting exercise independent of model assets.
struct Surface { float3 position; float3 normal; float3 albedo; float distance; float material; };

Output main(Input input) {
float2 uv=input.uv; Output o;

    const float3 cameraPosition=float3(0.0,-6.0,3.5);

    float3 forward=normalize(float3(0.0,0.0,0.8)-cameraPosition);
    float3 right=normalize(cross(forward,float3(0.0,0.0,1.0)));
    float3 up=cross(right,forward);
    float2 cameraCoord=(uv*2.0-1.0)*float2(p.screenSize.x/p.screenSize.y,-1.0);
    float3 rd=normalize(forward+0.55*(cameraCoord.x*right+cameraCoord.y*up));

    Surface s; s.distance=40.0; s.position=cameraPosition+rd*40.0;
    s.normal=float3(0.0,0.0,1.0); s.albedo=float3(0.04,0.08,0.14); s.material=-1.0;
    if(rd.z<-0.001) {
        float t=-cameraPosition.z/rd.z;
        if(t>0.0 && t<24.0) {
            s.distance=t; s.position=cameraPosition+rd*t;
            float checker=frac((floor(s.position.x)+floor(s.position.y))*0.5)*2.0;
            s.albedo=lerp((float3)(0.16),(float3)(0.32),checker); s.material=0.0;
        }
    }
    for(int i=0;i<3;i++) {
        float3 center=float3(float(i-1)*1.9,0.0,0.9);
        float3 oc=cameraPosition-center; float b=dot(oc,rd);
        float h=b*b-dot(oc,oc)+0.88*0.88;
        float t=h>0.0 ? -b-sqrt(h) : -1.0;
        if(t>0.0 && t<s.distance) {
            s.distance=t; s.position=cameraPosition+rd*t; s.normal=normalize(s.position-center);
            s.albedo=i==0 ? float3(0.9,0.15,0.06) : (i==1 ? float3(0.15,0.65,0.9) : float3(0.95,0.64,0.18));
            s.material=float(i+1);
        }
    }
float3 color=s.material<0.0 ? s.albedo : s.albedo*(0.1+max(dot(s.normal,normalize(float3(-0.3,-0.4,1.0))),0.0));
o.c0=float4(color,s.distance);

return o;
}
