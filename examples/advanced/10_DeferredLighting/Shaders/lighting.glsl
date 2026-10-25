#version 450
struct Params {
    vec2 screenSize;
    float time;
    uint lightCount;
    float exposure;
    float roughness;
    float metallic;
};
layout(push_constant) uniform Config { Params p; };
layout(location=0) in vec2 uv;
layout(location=0) out vec4 result0;
layout(set=0,binding=0) uniform texture2D positionBuffer;
layout(set=0,binding=1) uniform texture2D normalBuffer;
layout(set=0,binding=2) uniform texture2D albedoBuffer;
layout(set=0,binding=8) uniform sampler imageSampler;

struct Surface { vec3 position; vec3 normal; vec3 albedo; float distance; float material; };
void main() {
    vec4 position=texture(sampler2D(positionBuffer,imageSampler),uv), normal=texture(sampler2D(normalBuffer,imageSampler),uv), albedo=texture(sampler2D(albedoBuffer,imageSampler),uv);
    vec3 color=albedo.rgb;
    if(position.w>0.5) {
        vec3 n=normalize(normal.xyz),v=normalize(vec3(0.0,-6.0,3.5)-position.xyz);
        vec3 l=normalize(vec3(-0.4,-0.3,1.0)), base=albedo.rgb;
        float rough=normal.w, metal=albedo.w;
        vec3 h=normalize(v+l); float nl=max(dot(n,l),0.0),nv=max(dot(n,v),0.001);
        float nh=max(dot(n,h),0.0),vh=max(dot(v,h),0.0);
        float alpha=max(rough*rough,0.002), a2=alpha*alpha;
        float d=a2/(3.14159265*pow(nh*nh*(a2-1.0)+1.0,2.0));
        float k=(rough+1.0)*(rough+1.0)/8.0;
        float g=(nv/(nv*(1.0-k)+k))*(nl/(nl*(1.0-k)+k));
        vec3 f0=mix(vec3(0.04),base,metal), f=f0+(1.0-f0)*pow(1.0-vh,5.0);
        color=((1.0-f)*(1.0-metal)*base/3.14159265+d*g*f/max(4.0*nv*nl,0.001))*nl*3.0+albedo.rgb*0.04;
        // Multiple lights consume the same geometry buffers; no geometry redraw.
        Surface s; s.position=position.xyz;s.normal=n;s.albedo=albedo.rgb;s.distance=0.0;s.material=0.0;
        for(int i=0;i<int(p.lightCount);i++) {
            float f=float(i);
            vec3 position=vec3(sin(f*2.17+p.time*0.17)*4.0,cos(f*1.37+p.time*0.11)*3.0,0.5+fract(f*0.319)*2.5);
            vec3 delta=position-s.position; float d=length(delta), radius=2.4;
            float attenuation=pow(max(1.0-d/radius,0.0),2.0);
            color+=s.albedo*(0.4+0.6*cos(vec3(0.0,2.1,4.2)+float(i)*0.71))*max(dot(s.normal,delta/max(d,0.001)),0.0)*attenuation*1.8;
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
