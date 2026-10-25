#version 450
struct Params {
    vec2 screenSize;
    float exposure;
    float roughness;
    float metallic;
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
        float rough=clamp(p.roughness*(0.45+0.3*s.material),0.045,1.0);
        float metal=s.material>0.0 ? p.metallic : 0.0;
        vec3 l=normalize(vec3(-0.5,-0.3,1.0)), v=normalize(cameraPosition-s.position);
        vec3 n=s.normal, base=s.albedo;
        vec3 h=normalize(v+l); float nl=max(dot(n,l),0.0),nv=max(dot(n,v),0.001);
        float nh=max(dot(n,h),0.0),vh=max(dot(v,h),0.0);
        float alpha=max(rough*rough,0.002), a2=alpha*alpha;
        float d=a2/(3.14159265*pow(nh*nh*(a2-1.0)+1.0,2.0));
        float k=(rough+1.0)*(rough+1.0)/8.0;
        float g=(nv/(nv*(1.0-k)+k))*(nl/(nl*(1.0-k)+k));
        vec3 f0=mix(vec3(0.04),base,metal), f=f0+(1.0-f0)*pow(1.0-vh,5.0);
        color=((1.0-f)*(1.0-metal)*base/3.14159265+d*g*f/max(4.0*nv*nl,0.001))*nl*4.0+s.albedo*0.035;
    }
    vec3 c=color*p.exposure;
    c=clamp((c*(2.51*c+0.03))/(c*(2.43*c+0.59)+0.14),vec3(0.0),vec3(1.0));
    for(int channel=0;channel<3;channel++) {
        float x=max(c[channel],0.0);
        c[channel]=x<=0.0031308 ? x*12.92 : 1.055*pow(x,1.0/2.4)-0.055;
    }
    result0=vec4(c,1.0);
}
