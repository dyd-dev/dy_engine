#version 450
struct Params { vec4 sizeTime; vec4 settings; vec4 jitter; vec4 extra; };
layout(push_constant) uniform Config { Params p; };
layout(location=0) in vec2 uv;
layout(location=0) out vec4 result0;
layout(set=0,binding=0) uniform texture2D image0;
layout(set=0,binding=8) uniform sampler imageSampler;

// Analytic surfaces keep this lighting exercise independent of model assets.
struct Surface { vec3 position; vec3 normal; vec3 albedo; float distance; float material; };
void main() {
    const vec3 cameraPosition=vec3(0.0,-6.0,3.5);
    vec3 forward=normalize(vec3(0.0,0.0,0.8)-cameraPosition);
    vec3 right=normalize(cross(forward,vec3(0.0,0.0,1.0)));
    vec3 up=cross(right,forward);
    vec2 q=(uv*2.0-1.0)*vec2(p.sizeTime.x/p.sizeTime.y,-1.0);
    vec3 rd=normalize(forward+0.55*(q.x*right+q.y*up));
    Surface s; s.distance=40.0; s.position=cameraPosition+rd*40.0;
    s.normal=vec3(0.0,0.0,1.0); s.albedo=vec3(0.04,0.08,0.14); s.material=-1.0;
    if(rd.z<-0.001) {
        float t=-cameraPosition.z/rd.z;
        if(t>0.0 && t<24.0) {
            s.distance=t; s.position=cameraPosition+rd*t;
            float checker=fract((floor(s.position.x)+floor(s.position.y))*0.5)*2.0;
            s.albedo=mix(vec3(0.16),vec3(0.32),checker); s.material=0.0;
        }
    }
    for(int i=0;i<3;i++) {
        vec3 center=vec3(float(i-1)*1.9,0.0,0.9);
        vec3 oc=cameraPosition-center; float b=dot(oc,rd);
        float h=b*b-dot(oc,oc)+0.88*0.88;
        float t=h>0.0 ? -b-sqrt(h) : -1.0;
        if(t>0.0 && t<s.distance) {
            s.distance=t; s.position=cameraPosition+rd*t; s.normal=normalize(s.position-center);
            s.albedo=i==0 ? vec3(0.9,0.15,0.06) : (i==1 ? vec3(0.15,0.65,0.9) : vec3(0.95,0.64,0.18));
            s.material=float(i+1);
        }
    }
    // Invert the directional rays used by shadowDepth: their origins lie at
    // z=6 and their xy displacement per unit height is (-0.5,-0.3).
    vec2 shadowUV=(s.position.xy+(6.0-s.position.z)*vec2(0.5,0.3))/10.0+0.5;
    float receiver=(6.0-s.position.z)/8.0;
    vec3 lightDirection=normalize(vec3(0.5,0.3,1.0));
    float visibility=1.0;
    if(all(greaterThanEqual(shadowUV,vec2(0.0))) && all(lessThanEqual(shadowUV,vec2(1.0))) && receiver<=1.0) {
        vec2 pixel=1.0/vec2(textureSize(sampler2D(image0,imageSampler),0));
        float receiverBias=p.settings.y*(1.0+6.0*(1.0-max(dot(s.normal,lightDirection),0.0)));
        int radius=int(p.settings.z);
        float visibleSamples=0.0,samples=0.0;
        for(int y=-2;y<=2;y++) for(int x=-2;x<=2;x++) {
            if(abs(x)<=radius && abs(y)<=radius) {
                vec2 sampleUV=clamp(shadowUV+vec2(float(x),float(y))*pixel,pixel*0.5,1.0-pixel*0.5);
                float stored=textureLod(sampler2D(image0,imageSampler),sampleUV,0.0).r;
                visibleSamples+=receiver-receiverBias<=stored ? 1.0 : 0.0;
                samples+=1.0;
            }
        }
        if(samples>0.0) visibility=visibleSamples/samples;
    }
    vec3 color=s.material<0.0 ? s.albedo : s.albedo*(0.1+visibility*max(dot(s.normal,lightDirection),0.0));
    vec3 displayColor=max(color*p.settings.x,vec3(0.0));
    for(int channel=0; channel<3; channel++) {
        float value=displayColor[channel];
        displayColor[channel]=value<=0.0031308 ? value*12.92 : 1.055*pow(value,1.0/2.4)-0.055;
    }
    result0=vec4(displayColor,1.0);
}
