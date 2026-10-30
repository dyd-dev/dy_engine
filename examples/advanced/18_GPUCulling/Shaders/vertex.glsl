#version 450
layout(push_constant) uniform Frame {vec4 camera;vec4 projection;uvec4 controls;} frame;
layout(set=0,binding=0,std430) readonly buffer Objects {vec4 bounds[];};
layout(set=0,binding=1,std430) readonly buffer Visible {uint visibleIds[];};
layout(location=0) out vec3 color;
void main()
{
    const vec2 vertices[3]=vec2[3](vec2(-.8,-.6),vec2(.8,-.6),vec2(0,.95));
    uint id=frame.controls.z!=0u?visibleIds[gl_InstanceIndex]:frame.controls.w;
    vec4 sphere=bounds[id];float phase=.37*float(id);
    sphere.xy+=.25*vec2(sin(frame.camera.w+phase),cos(frame.camera.w*.8+phase));
    vec3 position=sphere.xyz-frame.camera.xyz;
    position.xy+=vertices[gl_VertexIndex]*sphere.w;
    float depth=frame.projection.w/(frame.projection.w-frame.projection.z);
    gl_Position=vec4(position.xy*frame.projection.xy,depth*(position.z-frame.projection.z),position.z);
    color=vec3(.15,.35,.65)+vec3(.75,.55,.3)*fract(float(id)*vec3(.13,.37,.07));
}
