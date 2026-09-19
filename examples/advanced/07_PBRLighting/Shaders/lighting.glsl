#version 450
struct Params { vec4 sizeTime; vec4 settings; vec4 jitter; vec4 extra; };
layout(push_constant) uniform Config { Params p; };
layout(location=0) in vec2 uv;
layout(location=0) out vec4 result0;

// Analytic surfaces keep this lighting exercise independent of model assets.
struct Surface { vec3 position; vec3 normal; vec3 albedo; float distance; float material; };
vec3 eye() { return vec3(0.0,-6.0,3.5); }
vec3 cameraRay(vec2 uv, vec2 size) {
    vec3 forward=normalize(vec3(0.0,0.0,0.8)-eye());
    vec3 right=normalize(cross(forward,vec3(0.0,0.0,1.0)));
    vec3 up=cross(right,forward);
    vec2 q=(uv*2.0-1.0)*vec2(size.x/size.y,-1.0);
    return normalize(forward+0.55*(q.x*right+q.y*up));
}
float sphereHit(vec3 origin,vec3 direction,vec3 center,float radius) {
    vec3 oc=origin-center; float b=dot(oc,direction);
    float h=b*b-dot(oc,oc)+radius*radius;
    return h>0.0 ? -b-sqrt(h) : -1.0;
}
Surface traceScene(vec3 rd) {
    Surface s; s.distance=40.0; s.position=eye()+rd*40.0;
    s.normal=vec3(0.0,0.0,1.0); s.albedo=vec3(0.04,0.08,0.14); s.material=-1.0;
    if(rd.z<-0.001) {
        float t=-eye().z/rd.z;
        if(t>0.0 && t<24.0) {
            s.distance=t; s.position=eye()+rd*t;
            float checker=fract((floor(s.position.x)+floor(s.position.y))*0.5)*2.0;
            s.albedo=mix(vec3(0.16),vec3(0.32),checker); s.material=0.0;
        }
    }
    for(int i=0;i<3;i++) {
        vec3 center=vec3(float(i-1)*1.9,0.0,0.9);
        float t=sphereHit(eye(),rd,center,0.88);
        if(t>0.0 && t<s.distance) {
            s.distance=t; s.position=eye()+rd*t; s.normal=normalize(s.position-center);
            s.albedo=i==0 ? vec3(0.9,0.15,0.06) : (i==1 ? vec3(0.15,0.65,0.9) : vec3(0.95,0.64,0.18));
            s.material=float(i+1);
        }
    }
    return s;
}
float SrgbChannel(float x) { x=max(x,0.0); return x<=0.0031308 ? x*12.92 : 1.055*pow(x,1.0/2.4)-0.055; }
vec3 srgb(vec3 c) { return vec3(SrgbChannel(c.r),SrgbChannel(c.g),SrgbChannel(c.b)); }
vec3 aces(vec3 c) { return clamp((c*(2.51*c+0.03))/(c*(2.43*c+0.59)+0.14),vec3(0.0),vec3(1.0)); }
vec3 brdf(vec3 n,vec3 v,vec3 l,vec3 base,float rough,float metal) {
    vec3 h=normalize(v+l); float nl=max(dot(n,l),0.0),nv=max(dot(n,v),0.001);
    float nh=max(dot(n,h),0.0),vh=max(dot(v,h),0.0);
    float alpha=max(rough*rough,0.002), a2=alpha*alpha;
    float d=a2/(3.14159265*pow(nh*nh*(a2-1.0)+1.0,2.0));
    float k=(rough+1.0)*(rough+1.0)/8.0;
    float g=(nv/(nv*(1.0-k)+k))*(nl/(nl*(1.0-k)+k));
    vec3 f0=mix(vec3(0.04),base,metal), f=f0+(1.0-f0)*pow(1.0-vh,5.0);
    return ((1.0-f)*(1.0-metal)*base/3.14159265+d*g*f/max(4.0*nv*nl,0.001))*nl;
}

void main() {

Surface s=traceScene(cameraRay(uv,p.sizeTime.xy));
vec3 color=s.albedo;
if(s.material>=0.0) {
    float rough=clamp(p.settings.y*(0.45+0.3*s.material),0.045,1.0);
    float metal=s.material>0.0 ? p.settings.z : 0.0;
    vec3 l=normalize(vec3(-0.5,-0.3,1.0)), v=normalize(eye()-s.position);
    color=brdf(s.normal,v,l,s.albedo,rough,metal)*4.0+s.albedo*0.035;
    if(p.sizeTime.w==1.0) color=s.albedo*(0.04+max(dot(s.normal,l),0.0));
    if(p.sizeTime.w==2.0) color=s.normal*0.5+0.5;
}
result0=vec4(srgb(aces(color*p.settings.x)),1.0);

}
