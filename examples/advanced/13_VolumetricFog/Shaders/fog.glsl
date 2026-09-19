#version 450
struct Params { vec4 sizeTime; vec4 settings; vec4 jitter; vec4 extra; };
layout(push_constant) uniform Config { Params p; };
layout(location=0) in vec2 uv;
layout(location=0) out vec4 result0;
layout(set=0,binding=0) uniform texture2D image0;
#define SAMPLE0(U) texture(sampler2D(image0,imageSampler),U)
layout(set=0,binding=8) uniform sampler imageSampler;

vec3 eye() { return vec3(0.0,-6.0,3.5); }
vec3 cameraRay(vec2 uv, vec2 size) {
    vec3 forward=normalize(vec3(0.0,0.0,0.8)-eye());
    vec3 right=normalize(cross(forward,vec3(0.0,0.0,1.0)));
    vec3 up=cross(right,forward);
    vec2 q=(uv*2.0-1.0)*vec2(size.x/size.y,-1.0);
    return normalize(forward+0.55*(q.x*right+q.y*up));
}

float SrgbChannel(float x) { x=max(x,0.0); return x<=0.0031308 ? x*12.92 : 1.055*pow(x,1.0/2.4)-0.055; }
vec3 srgb(vec3 c) { return vec3(SrgbChannel(c.r),SrgbChannel(c.g),SrgbChannel(c.b)); }
vec3 aces(vec3 c) { return clamp((c*(2.51*c+0.03))/(c*(2.43*c+0.59)+0.14),vec3(0.0),vec3(1.0)); }

void main() {

vec4 scene=SAMPLE0(uv);vec3 rd=cameraRay(uv,p.sizeTime.xy);
int steps=int(p.extra.y);float distance=min(scene.a,18.0),dt=distance/float(steps);
float transmittance=1.0;vec3 scattering=vec3(0.0);
float g=0.35,mu=dot(rd,normalize(vec3(-0.3,0.5,1.0)));
float phase=(1.0-g*g)/(12.5663706*pow(1.0+g*g-2.0*g*mu,1.5));
// Midpoint integration of Beer-Lambert extinction through procedural density.
for(int i=0;i<256;i++) {
    if(i>=steps) break;
    vec3 pos=eye()+rd*((float(i)+0.5)*dt);
    float noise=0.65+0.35*sin(pos.x*2.1+p.sizeTime.z*0.3)*sin(pos.y*1.7)*sin(pos.z*2.9);
    float density=p.extra.x*exp(-max(pos.z,0.0)*p.settings.y)*noise;
    float stepTransmission=exp(-density*dt);
    scattering+=transmittance*(1.0-stepTransmission)*vec3(0.5,0.7,1.0)*(0.35+phase*5.0);
    transmittance*=stepTransmission;
}
vec3 color=scene.rgb*transmittance+scattering;
if(p.sizeTime.w==1.0) color=scene.rgb;
if(p.sizeTime.w==2.0) color=vec3(transmittance);
if(p.sizeTime.w==3.0) color=scattering;
result0=vec4(srgb(aces(color*p.settings.x)),1.0);

}
