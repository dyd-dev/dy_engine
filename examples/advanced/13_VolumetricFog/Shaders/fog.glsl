#version 450
struct Params {
    vec2 screenSize;
    float time;
    float exposure;
    float density;
    float heightFalloff;
    uint stepCount;
};
layout(push_constant) uniform Config { Params p; };
layout(location=0) in vec2 uv;
layout(location=0) out vec4 result0;
layout(set=0,binding=0) uniform texture2D sceneColor;
layout(set=0,binding=8) uniform sampler imageSampler;

void main() {

    vec4 scene=texture(sampler2D(sceneColor,imageSampler),uv);
    const vec3 cameraPosition=vec3(0.0,-6.0,3.5);

    vec3 forward=normalize(vec3(0.0,0.0,0.8)-cameraPosition);
    vec3 right=normalize(cross(forward,vec3(0.0,0.0,1.0)));
    vec3 up=cross(right,forward);
    vec2 cameraCoord=(uv*2.0-1.0)*vec2(p.screenSize.x/p.screenSize.y,-1.0);
    vec3 rd=normalize(forward+0.55*(cameraCoord.x*right+cameraCoord.y*up));
int steps=int(p.stepCount);float distance=min(scene.a,18.0),dt=distance/float(steps);
float transmittance=1.0;vec3 scattering=vec3(0.0);
float g=0.35,mu=dot(rd,normalize(vec3(-0.3,0.5,1.0)));
float phase=(1.0-g*g)/(12.5663706*pow(1.0+g*g-2.0*g*mu,1.5));
// Midpoint integration of Beer-Lambert extinction through procedural density.
for(int i=0;i<256;i++) {
    if(i>=steps) break;
    vec3 pos=cameraPosition+rd*((float(i)+0.5)*dt);
    float noise=0.65+0.35*sin(pos.x*2.1+p.time*0.3)*sin(pos.y*1.7)*sin(pos.z*2.9);
    float density=p.density*exp(-max(pos.z,0.0)*p.heightFalloff)*noise;
    float stepTransmission=exp(-density*dt);
    scattering+=transmittance*(1.0-stepTransmission)*vec3(0.5,0.7,1.0)*(0.35+phase*5.0);
    transmittance*=stepTransmission;
}
vec3 color=scene.rgb*transmittance+scattering;
    color*=p.exposure;
    color=clamp((color*(2.51*color+0.03))/(color*(2.43*color+0.59)+0.14),vec3(0.0),vec3(1.0));
    for(int channel=0;channel<3;channel++) {
        float x=max(color[channel],0.0);
        color[channel]=x<=0.0031308 ? x*12.92 : 1.055*pow(x,1.0/2.4)-0.055;
    }
    result0=vec4(color,1.0);

}
