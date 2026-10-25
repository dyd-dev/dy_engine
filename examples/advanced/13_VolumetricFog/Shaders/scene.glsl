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

// Analytic surfaces keep this lighting exercise independent of model assets.
struct Surface { vec3 position; vec3 normal; vec3 albedo; float distance; float material; };

void main() {

    const vec3 cameraPosition=vec3(0.0,-6.0,3.5);

    vec3 forward=normalize(vec3(0.0,0.0,0.8)-cameraPosition);
    vec3 right=normalize(cross(forward,vec3(0.0,0.0,1.0)));
    vec3 up=cross(right,forward);
    vec2 cameraCoord=(uv*2.0-1.0)*vec2(p.screenSize.x/p.screenSize.y,-1.0);
    vec3 rd=normalize(forward+0.55*(cameraCoord.x*right+cameraCoord.y*up));

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
vec3 color=s.material<0.0 ? s.albedo : s.albedo*(0.1+max(dot(s.normal,normalize(vec3(-0.3,-0.4,1.0))),0.0));
result0=vec4(color,s.distance);

}
