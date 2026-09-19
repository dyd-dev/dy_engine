struct Params { float4 sizeTime; float4 settings; float4 jitter; float4 extra; };
struct Input { float4 position :SV_Position; float2 uv :TEXCOORD0; };
struct Output { float4 c0 : SV_Target0; float4 c1 : SV_Target1; };
cbuffer Config:register(b15) { Params p; };

Output main(Input input) {
float2 uv=input.uv; Output o;

// A moving procedural foreground over a static high-frequency grid.
// Motion is the previous unjittered UV minus the current unjittered UV.
float2 sampleUV=uv+p.jitter.xy/p.sizeTime.xy;
float2 center=float2(0.5+0.22*sin(p.sizeTime.z),0.48);
float2 previousCenter=float2(0.5+0.22*sin(p.extra.w),0.48);
float2 q=(sampleUV-center)*float2(p.sizeTime.x/p.sizeTime.y,1.0);
bool foreground=dot(q,q)<0.025;
float grid=frac((floor(sampleUV.x*170.0)+floor(sampleUV.y*110.0))*0.5)*2.0;
float3 color=lerp(float3(0.04,0.08,0.16),float3(0.65,0.8,0.92),grid);
if(foreground) color=float3(0.95,0.2,0.06)*(0.6+0.4*cos(sampleUV.x*1000.0));
float2 velocity=foreground ? previousCenter-center : (float2)(0.0);
o.c0=float4(color,foreground ? 0.3 : 0.9);
o.c1=float4(velocity,0.0,1.0);

return o;
}
