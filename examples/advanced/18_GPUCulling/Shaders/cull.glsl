#version 450
layout(local_size_x=64,local_size_y=1,local_size_z=1) in;
layout(push_constant) uniform Frame {vec4 camera;vec4 projection;uvec4 controls;} frame;
layout(set=0,binding=0,std430) readonly buffer Objects {vec4 bounds[];};
layout(set=0,binding=1,std430) buffer Visible {uint visibleIds[];};
layout(set=0,binding=2,std430) buffer Arguments {uint arguments[];};
void main()
{
    uint id=gl_GlobalInvocationID.x;
    if(frame.controls.y==0u)
    {
        if(id==0u) {arguments[0]=3u;arguments[1]=0u;arguments[2]=0u;arguments[3]=0u;}
        return;
    }
    if(id>=frame.controls.x) return;
    vec4 sphere=bounds[id];float phase=.37*float(id);
    sphere.xy+=.25*vec2(sin(frame.camera.w+phase),cos(frame.camera.w*.8+phase));
    vec3 position=sphere.xyz-frame.camera.xyz;
    vec2 margin=sphere.w*sqrt(frame.projection.xy*frame.projection.xy+vec2(1));
    bool inside=position.z+sphere.w>=frame.projection.z && position.z-sphere.w<=frame.projection.w &&
        all(lessThanEqual(abs(position.xy)*frame.projection.xy,vec2(position.z)+margin));
    if(!inside) return;
    uint slot=atomicAdd(arguments[1],1u);
    visibleIds[slot]=id;
}
