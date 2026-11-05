#version 450
layout(local_size_x=64) in;
layout(std430,set=0,binding=0) readonly buffer Arguments {uint words[];};
layout(rgba8,set=0,binding=1) writeonly uniform image2D outputImage;
struct Instance {mat4 model,previous;};
layout(std430,set=0,binding=2) readonly buffer Instances {Instance instances[];};
layout(push_constant) uniform Count {uvec4 counts;};
void main() {
    uint i=gl_GlobalInvocationID.x;
    if(i>=counts.x)return;
    uint value;
    if(counts.y==0)value=words[i];
    else {
        uint row=i/32,component=i%32,k=component%16;
        mat4 m=component<16?instances[row].model:instances[row].previous;
        value=floatBitsToUint(m[k/4][k%4]);
    }
    imageStore(outputImage,ivec2(i,0),vec4(value&255u,(value>>8u)&255u,(value>>16u)&255u,value>>24u)/255.0);
}
