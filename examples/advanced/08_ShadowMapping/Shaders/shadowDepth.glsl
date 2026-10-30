#version 450
layout(location=0) in vec2 uv;
void main() {
    vec3 origin=vec3((uv-0.5)*10.0,6.0),rd=normalize(vec3(-0.5,-0.3,-1.0));
    float closest=20.0;
    if(rd.z<-0.001) closest=min(closest,-origin.z/rd.z);
    for(int i=0; i<3; i++) {
        vec3 oc=origin-(vec3(float(i-1)*1.9,0.0,0.9));
        float b=dot(oc,rd);
        float h=b*b-dot(oc,oc)+(0.88)*(0.88);
        float t=h>0.0 ? -b-sqrt(h) : -1.0;
        if(t>0.0) closest=min(closest,t);
    }
    vec3 hit=origin+rd*closest;
    gl_FragDepth=clamp((6.0-hit.z)/8.0,0.0,1.0);
}
