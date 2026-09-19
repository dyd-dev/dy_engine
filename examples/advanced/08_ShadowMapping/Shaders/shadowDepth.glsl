#version 450
struct Params { vec4 sizeTime; vec4 settings; vec4 jitter; vec4 extra; };
layout(push_constant) uniform Config { Params p; };
layout(location=0) in vec2 uv;

float sphereHit(vec3 origin,vec3 direction,vec3 center,float radius) {
    vec3 oc=origin-center; float b=dot(oc,direction);
    float h=b*b-dot(oc,oc)+radius*radius;
    return h>0.0 ? -b-sqrt(h) : -1.0;
}

void main() {

// Each atlas tile has its own light projection; fragment depth stores comparable distance.
vec3 origin=vec3((uv-0.5)*10.0,6.0),rd=normalize(vec3(-0.5,-0.3,-1.0));
if(p.settings.w==1.0) {
    float cascade=floor(uv.x*2.0);vec2 local=vec2(fract(uv.x*2.0),uv.y);
    float extent=cascade<0.5 ? 12.0 : 28.0;
    origin=vec3((local-0.5)*extent+vec2(0.0,1.0),6.0);
}
if(p.settings.w==2.0) {
    vec2 tile=floor(uv*vec2(3.0,2.0)),q=fract(uv*vec2(3.0,2.0))*2.0-1.0;
    int face=int(tile.x+tile.y*3.0);
    if(face==0) rd=vec3(1.0,-q.y,-q.x);
    if(face==1) rd=vec3(-1.0,-q.y,q.x);
    if(face==2) rd=vec3(q.x,1.0,q.y);
    if(face==3) rd=vec3(q.x,-1.0,-q.y);
    if(face==4) rd=vec3(q.x,-q.y,1.0);
    if(face==5) rd=vec3(-q.x,-q.y,-1.0);
    origin=vec3(0.0,-2.0,p.extra.y);rd=normalize(rd);
}
float closest=20.0;
if(rd.z<-0.001) closest=min(closest,-origin.z/rd.z);
for(int i=0;i<3;i++) {
    float t=sphereHit(origin,rd,vec3(float(i-1)*1.9,0.0,0.9),0.88);
    if(t>0.0) closest=min(closest,t);
}
vec3 hit=origin+rd*closest;
gl_FragDepth=p.settings.w==2.0 ? closest/20.0 : clamp((6.0-hit.z)/8.0,0.0,1.0);

}
