#version 450
struct Params { vec4 sizeTime; vec4 settings; vec4 jitter; vec4 extra; };
layout(push_constant) uniform Config { Params p; };
layout(location=0) in vec2 uv;
layout(location=0) out vec4 result0;
layout(location=1) out vec4 result1;

void main() {

// A moving procedural foreground over a static high-frequency grid.
// Motion is the previous unjittered UV minus the current unjittered UV.
vec2 sampleUV=uv+p.jitter.xy/p.sizeTime.xy;
vec2 center=vec2(0.5+0.22*sin(p.sizeTime.z),0.48);
vec2 previousCenter=vec2(0.5+0.22*sin(p.extra.w),0.48);
vec2 q=(sampleUV-center)*vec2(p.sizeTime.x/p.sizeTime.y,1.0);
bool foreground=dot(q,q)<0.025;
float grid=fract((floor(sampleUV.x*170.0)+floor(sampleUV.y*110.0))*0.5)*2.0;
vec3 color=mix(vec3(0.04,0.08,0.16),vec3(0.65,0.8,0.92),grid);
if(foreground) color=vec3(0.95,0.2,0.06)*(0.6+0.4*cos(sampleUV.x*1000.0));
vec2 velocity=foreground ? previousCenter-center : vec2(0.0);
result0=vec4(color,foreground ? 0.3 : 0.9);
result1=vec4(velocity,0.0,1.0);

}
