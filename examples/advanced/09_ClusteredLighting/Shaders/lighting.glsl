#version 450
struct Params {
    vec2 screenSize;
    float time;
    uint lightCount;
    float exposure;
};
layout(push_constant) uniform Config { Params p; };
layout(location=0) in vec2 uv;
layout(location=0) out vec4 result0;
layout(set=0,binding=0,std430) readonly buffer LightLists { uint indices[]; };

// Analytic surfaces keep this lighting exercise independent of model assets.
struct Surface { vec3 position; vec3 normal; vec3 albedo; float distance; float material; };
void main() {
    const vec3 cameraPosition=vec3(0.0,-6.0,3.5);
    vec3 forward=normalize(vec3(0.0,0.0,0.8)-cameraPosition);
    vec3 right=normalize(cross(forward,vec3(0.0,0.0,1.0)));
    vec3 up=cross(right,forward);
    vec2 q=(uv*2.0-1.0)*vec2(p.screenSize.x/p.screenSize.y,-1.0);
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
    vec3 color=s.albedo;
    if(s.material>=0.0) {
        // World-space clusters cover [-6,6] x [-6,6] x [0,4].
        vec3 normalized=(s.position-vec3(-6.0,-6.0,0.0))/vec3(12.0,12.0,4.0);
        ivec3 cell=ivec3(clamp(floor(normalized*vec3(16.0,16.0,8.0)),vec3(0.0),vec3(15.0,15.0,7.0)));
        uint cluster=uint(cell.x+16*(cell.y+16*cell.z)), base=cluster*65u;
        uint count=indices[base];color=s.albedo*0.04;
        bool outside=any(lessThan(normalized,vec3(0.0))) || any(greaterThanEqual(normalized,vec3(1.0)));
        if(outside) {
            for(int i=0;i<int(p.lightCount);i++) {
            float f=float(i);
            vec3 position=vec3(sin(f*2.17+p.time*0.17)*4.0,cos(f*1.37+p.time*0.11)*3.0,0.5+fract(f*0.319)*2.5);
            vec3 delta=position-s.position; float d=length(delta), radius=2.4;
            float attenuation=pow(max(1.0-d/radius,0.0),2.0);
            color+=s.albedo*(0.4+0.6*cos(vec3(0.0,2.1,4.2)+float(i)*0.71))*max(dot(s.normal,delta/max(d,0.001)),0.0)*attenuation*1.8;
        }
        } else {
            for(uint j=0u;j<count;j++) {
            int i=int(indices[base+1u+j]); float f=float(i);
            vec3 position=vec3(sin(f*2.17+p.time*0.17)*4.0,cos(f*1.37+p.time*0.11)*3.0,0.5+fract(f*0.319)*2.5);
            vec3 delta=position-s.position; float d=length(delta), radius=2.4;
            float attenuation=pow(max(1.0-d/radius,0.0),2.0);
            color+=s.albedo*(0.4+0.6*cos(vec3(0.0,2.1,4.2)+float(i)*0.71))*max(dot(s.normal,delta/max(d,0.001)),0.0)*attenuation*1.8;
        }
        }
    }
    vec3 c=color*p.exposure;
    c=clamp((c*(2.51*c+0.03))/(c*(2.43*c+0.59)+0.14),vec3(0.0),vec3(1.0));
    for(int channel=0;channel<3;channel++) {
        float x=max(c[channel],0.0);
        c[channel]=x<=0.0031308 ? x*12.92 : 1.055*pow(x,1.0/2.4)-0.055;
    }
    result0=vec4(c,1.0);
}
