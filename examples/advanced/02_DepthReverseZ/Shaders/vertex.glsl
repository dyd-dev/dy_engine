#version 450
layout(location=0) out vec3 color;
layout(push_constant) uniform Settings {vec4 value;} settings;
void main(){vec2 corners[6]=vec2[](vec2(-1,-1),vec2(1,-1),vec2(-1,1),vec2(-1,1),vec2(1,-1),vec2(1,1));
int instance=gl_InstanceIndex;bool nearFace=(instance&1)==1;bool distant=instance>=2;
float n=settings.value.z,f=settings.value.w;
float z=distant?10000.0+(nearFace?0.0:.002):(nearFace?2.0:3.0);
vec2 center=vec2(distant?.48:-.48,nearFace?.06:-.06);
vec2 ndc=center+corners[gl_VertexIndex]*vec2(.34,.5);
float clipDepth=settings.value.x>.5?(f*n-n*z)/(f-n):(f*z-f*n)/(f-n);
gl_Position=vec4(ndc*z,clipDepth,z);color=nearFace?vec3(.1,.85,.35):vec3(.9,.12,.2);}
