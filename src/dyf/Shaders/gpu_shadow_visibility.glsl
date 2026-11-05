#version 450
layout(local_size_x=64) in;
struct Input {vec4 minimum,maximum;uvec4 ids;};
struct Batch {uint indexCount,firstIndex;int vertexOffset;uint instanceOffset;uvec4 capacityPadding;};
struct View {mat4 projection;vec4 receiverPlanes[18];uvec4 info;};
struct Instance {mat4 model,previous;};
layout(std430,set=0,binding=0) readonly buffer Inputs {Input inputs[];};
layout(std430,set=0,binding=1) readonly buffer Batches {Batch batches[];};
layout(std430,set=0,binding=2) readonly buffer Views {View views[];};
layout(std430,set=0,binding=3) readonly buffer Models {Instance models[];};
layout(std430,set=0,binding=4) writeonly buffer Visible {Instance visible[];};
layout(std430,set=0,binding=5) buffer Arguments {uint arguments[];};
layout(push_constant) uniform Operation {uvec4 counts;uvec4 settings;};
bool finite4(vec4 value) {return !any(isnan(value)) && !any(isinf(value));}
bool visibleToLight(Input item,mat4 model,View view) {
    if(!(item.minimum.w>0) || !finite4(item.minimum) || !finite4(item.maximum) ||
        any(greaterThan(item.minimum.xyz,item.maximum.xyz)))return true;
    for(uint i=0;i<4;++i)if(!finite4(model[i]) || !finite4(view.projection[i]))return true;
    if(model[0].w!=0 || model[1].w!=0 || model[2].w!=0 || model[3].w!=1)return true;
    vec3 center=(item.minimum.xyz+item.maximum.xyz)*.5,halfSize=(item.maximum.xyz-item.minimum.xyz)*.5;
    vec3 localMagnitude=max(abs(item.minimum.xyz),abs(item.maximum.xyz));
    vec3 worldCenter=(model*vec4(center,1)).xyz;
    vec3 extent=abs(model[0].xyz)*halfSize.x+abs(model[1].xyz)*halfSize.y+abs(model[2].xyz)*halfSize.z;
    vec3 magnitude=abs(model[3].xyz)+abs(model[0].xyz)*localMagnitude.x+
        abs(model[1].xyz)*localMagnitude.y+abs(model[2].xyz)*localMagnitude.z;
    vec3 low=worldCenter-extent,high=worldCenter+extent;
    if(!finite4(vec4(low,0)) || !finite4(vec4(high,0)) || any(greaterThan(magnitude,vec3(1e21))))return true;
    mat4 rows=transpose(view.projection);
    for(uint i=0;i<6+view.info.x;++i) {
        vec4 plane,scalePlane;
        if(i<6) {
            plane=i==4?rows[2]:rows[3]+(i%2==0?1.0:-1.0)*rows[i/2];
            scalePlane=i==4?abs(rows[2]):abs(rows[3])+abs(rows[i/2]);
            if(all(equal(plane.xyz,vec3(0))))return true;
        } else {plane=view.receiverPlanes[i-6];scalePlane=abs(plane);}
        float distance=plane.w+dot(plane.xyz,mix(low,high,greaterThanEqual(plane.xyz,vec3(0))));
        float scale=scalePlane.w+dot(scalePlane.xyz,magnitude);
        if(!finite4(plane) || !finite4(vec4(distance,scale,0,0)) || scale>1e32)return true;
        // Cover f32 plane conversion, transformed-bound rounding and fused operations.
        if(distance < -1e-4*max(1.0,scale))return false;
    }
    return true;
}
void main() {
    uint id=gl_GlobalInvocationID.x+gl_GlobalInvocationID.y*counts.y;
    if(counts.x==0) {
        if(id>=counts.w*settings.x)return;
        uint view=id/counts.w,b=id%counts.w;Batch batch=batches[b];uint offset=id*5;
        arguments[offset]=batch.indexCount;arguments[offset+1]=0;arguments[offset+2]=batch.firstIndex;
        arguments[offset+3]=uint(batch.vertexOffset);arguments[offset+4]=view*settings.y+batch.instanceOffset;
    } else {
        if(id>=counts.z*settings.x)return;
        uint view=id/counts.z;if(views[view].info.y==0)return;
        Input item=inputs[id%counts.z];Instance instance=models[item.ids.x];
        if(!visibleToLight(item,instance.model,views[view]))return;
        Batch batch=batches[item.ids.y];uint offset=(view*counts.w+item.ids.y)*5;
        uint slot=atomicAdd(arguments[offset+1],1);
        if(slot>=batch.capacityPadding.x) {atomicMin(arguments[offset+1],batch.capacityPadding.x);return;}
        visible[view*settings.y+batch.instanceOffset+slot]=instance;
    }
}
