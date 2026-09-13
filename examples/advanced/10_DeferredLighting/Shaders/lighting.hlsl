struct Params { float4 sizeTime; float4 settings; float4 jitter; float4 extra; };
struct Input { float4 position :SV_Position; float2 uv :TEXCOORD0; };
struct Output { float4 c0 : SV_Target0; };
cbuffer Config:register(b15) { Params p; };
Texture2D<float4> image0:register(t0);
#define SAMPLE0(U) image0.SampleLevel(imageSampler,U,0)
Texture2D<float4> image1:register(t1);
#define SAMPLE1(U) image1.SampleLevel(imageSampler,U,0)
Texture2D<float4> image2:register(t2);
#define SAMPLE2(U) image2.SampleLevel(imageSampler,U,0)
SamplerState imageSampler:register(s8);

struct Surface { float3 position; float3 normal; float3 albedo; float distance; float material; };
float3 eye() { return float3(0.0,-6.0,3.5); }

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
float3 lightPosition(int i,float time) {
    float f=float(i); return float3(sin(f*2.17+time*0.17)*4.0,cos(f*1.37+time*0.11)*3.0,0.5+frac(f*0.319)*2.5);
}
float3 lightColor(int i) { return 0.4+0.6*cos(float3(0.0,2.1,4.2)+float(i)*0.71); }
float3 pointLight(Surface s,int i,float time) {
    float3 delta=lightPosition(i,time)-s.position; float d=length(delta), radius=2.4;
    float attenuation=pow(max(1.0-d/radius,0.0),2.0);
    return s.albedo*lightColor(i)*max(dot(s.normal,delta/max(d,0.001)),0.0)*attenuation*1.8;
}

Output main(Input input) {
float2 uv=input.uv; Output o;

float4 position=SAMPLE0(uv), normal=SAMPLE1(uv), albedo=SAMPLE2(uv);
float3 color=albedo.rgb;
if(position.w>0.5) {
    float3 n=normalize(normal.xyz),v=normalize(eye()-position.xyz);
    color=brdf(n,v,normalize(float3(-0.4,-0.3,1.0)),albedo.rgb,normal.w,albedo.w)*3.0+albedo.rgb*0.04;
    // Multiple lights consume the same geometry buffers; no geometry redraw.
    Surface s; s.position=position.xyz;s.normal=n;s.albedo=albedo.rgb;s.distance=0.0;s.material=0.0;
    for(int i=0;i<int(p.extra.z);i++) color+=pointLight(s,i,p.sizeTime.z);
}
if(p.sizeTime.w==1.0) color=normal.xyz*0.5+0.5;
if(p.sizeTime.w==2.0) color=albedo.rgb;
if(p.sizeTime.w==3.0) color=position.xyz*0.1+0.5;
o.c0=float4(srgb(aces(color*p.settings.x)),1.0);

return o;
}
