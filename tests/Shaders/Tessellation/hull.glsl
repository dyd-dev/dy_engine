#version 450

layout(vertices = 3) out;
layout(location = 0) in vec2 controlPosition[];
layout(location = 0) out vec2 tessellatedPosition[];

void main()
{
    tessellatedPosition[gl_InvocationID] = controlPosition[gl_InvocationID];
    if(gl_InvocationID == 0)
    {
        gl_TessLevelOuter[0] = 4.0;
        gl_TessLevelOuter[1] = 4.0;
        gl_TessLevelOuter[2] = 4.0;
        gl_TessLevelInner[0] = 4.0;
    }
}
