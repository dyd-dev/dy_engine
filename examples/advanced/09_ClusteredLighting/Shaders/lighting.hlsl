bool2 lessThan(float2 a,float2 b) { return a<b; }
bool3 lessThan(float3 a,float3 b) { return a<b; }

bool2 greaterThanEqual(float2 a,float2 b) { return a>=b; }
bool3 greaterThanEqual(float3 a,float3 b) { return a>=b; }

struct Params {
    float2 screenSize;
    float time;
    uint lightCount;
    float exposure;
};
struct Input { float4 position :SV_Position; float2 uv :TEXCOORD0; };
struct Output { float4 c0 : SV_Target0; };
cbuffer Config:register(b15) { Params p; };
StructuredBuffer<uint> indices:register(t0);

// Analytic surfaces keep this lighting exercise independent of model assets.
struct Surface { float3 position; float3 normal; float3 albedo; float distance; float material; };
Output main(Input input) {
    float2 uv=input.uv; Output o;
    const float3 cameraPosition=float3(0.0,-6.0,3.5);
    float3 forward=normalize(float3(0.0,0.0,0.8)-cameraPosition);
    float3 right=normalize(cross(forward,float3(0.0,0.0,1.0)));
    float3 up=cross(right,forward);
    float2 q=(uv*2.0-1.0)*float2(p.screenSize.x/p.screenSize.y,-1.0);
    float3 rd=normalize(forward+0.55*(q.x*right+q.y*up));
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
    float3 color=s.albedo;
    if(s.material>=0.0) {
        // World-space clusters cover [-6,6] x [-6,6] x [0,4].
        float3 normalized=(s.position-float3(-6.0,-6.0,0.0))/float3(12.0,12.0,4.0);
        int3 cell=(int3)(clamp(floor(normalized*float3(16.0,16.0,8.0)),(float3)(0.0),float3(15.0,15.0,7.0)));
        uint cluster=uint(cell.x+16*(cell.y+16*cell.z)), base=cluster*65u;
        uint count=indices[base];color=s.albedo*0.04;
        bool outside=any(lessThan(normalized,(float3)(0.0))) || any(greaterThanEqual(normalized,(float3)(1.0)));
        if(outside) {
            for(int i=0;i<int(p.lightCount);i++) {
            float f=float(i);
            float3 position=float3(sin(f*2.17+p.time*0.17)*4.0,cos(f*1.37+p.time*0.11)*3.0,0.5+frac(f*0.319)*2.5);
            float3 delta=position-s.position; float d=length(delta), radius=2.4;
            float attenuation=pow(max(1.0-d/radius,0.0),2.0);
            color+=s.albedo*(0.4+0.6*cos(float3(0.0,2.1,4.2)+float(i)*0.71))*max(dot(s.normal,delta/max(d,0.001)),0.0)*attenuation*1.8;
        }
        } else {
            for(uint j=0u;j<count;j++) {
            int i=int(indices[base+1u+j]); float f=float(i);
            float3 position=float3(sin(f*2.17+p.time*0.17)*4.0,cos(f*1.37+p.time*0.11)*3.0,0.5+frac(f*0.319)*2.5);
            float3 delta=position-s.position; float d=length(delta), radius=2.4;
            float attenuation=pow(max(1.0-d/radius,0.0),2.0);
            color+=s.albedo*(0.4+0.6*cos(float3(0.0,2.1,4.2)+float(i)*0.71))*max(dot(s.normal,delta/max(d,0.001)),0.0)*attenuation*1.8;
        }
        }
    }
    float3 c=color*p.exposure;
    c=clamp((c*(2.51*c+0.03))/(c*(2.43*c+0.59)+0.14),(float3)(0.0),(float3)(1.0));
    for(int channel=0;channel<3;channel++) {
        float x=max(c[channel],0.0);
        c[channel]=x<=0.0031308 ? x*12.92 : 1.055*pow(x,1.0/2.4)-0.055;
    }
    o.c0=float4(c,1.0);

    return o;
}
