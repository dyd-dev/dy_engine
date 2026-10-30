struct Params { float4 sizeTime; float4 settings; float4 jitter; float4 extra; };
struct Input { float4 position :SV_Position; float2 uv :TEXCOORD0; };
struct Output { float4 c0 : SV_Target0; };
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
float SrgbChannel(float x) { x=max(x,0.0); return x<=0.0031308 ? x*12.92 : 1.055*pow(x,1.0/2.4)-0.055; }
float3 srgb(float3 c) { return float3(SrgbChannel(c.r),SrgbChannel(c.g),SrgbChannel(c.b)); }
float3 aces(float3 c) { return clamp((c*(2.51*c+0.03))/(c*(2.43*c+0.59)+0.14),(float3)(0.0),(float3)(1.0)); }
float3 brdf(float3 n,float3 v,float3 l,float3 base,float rough,float metal) {
    float3 h=normalize(v+l); float nl=max(dot(n,l),0.0),nv=max(dot(n,v),0.001);
    float nh=max(dot(n,h),0.0),vh=max(dot(v,h),0.0);
    float alpha=max(rough*rough,0.002), a2=alpha*alpha;
    float d=a2/(3.14159265*pow(nh*nh*(a2-1.0)+1.0,2.0));
    float k=(rough+1.0)*(rough+1.0)/8.0;
    float g=(nv/(nv*(1.0-k)+k))*(nl/(nl*(1.0-k)+k));
    float3 f0=lerp((float3)(0.04),base,metal), f=f0+(1.0-f0)*pow(1.0-vh,5.0);
    return ((1.0-f)*(1.0-metal)*base/3.14159265+d*g*f/max(4.0*nv*nl,0.001))*nl;
}

Output main(Input input) {
float2 uv=input.uv; Output o;

Surface s=traceScene(cameraRay(uv,p.sizeTime.xy));
float3 color=s.albedo;
if(s.material>=0.0) {
    float rough=clamp(p.settings.y*(0.45+0.3*s.material),0.045,1.0);
    float metal=s.material>0.0 ? p.settings.z : 0.0;
    float3 l=normalize(float3(-0.5,-0.3,1.0)), v=normalize(eye()-s.position);
    color=brdf(s.normal,v,l,s.albedo,rough,metal)*4.0+s.albedo*0.035;
    if(p.sizeTime.w==1.0) color=s.albedo*(0.04+max(dot(s.normal,l),0.0));
    if(p.sizeTime.w==2.0) color=s.normal*0.5+0.5;
}
o.c0=float4(srgb(aces(color*p.settings.x)),1.0);

return o;
}
