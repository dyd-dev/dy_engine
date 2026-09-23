#version 450

layout(triangles, equal_spacing, ccw) in;
layout(location = 0) in vec2 tessellatedPosition[];

void main()
{
    const vec2 position = tessellatedPosition[0] * gl_TessCoord.x
        + tessellatedPosition[1] * gl_TessCoord.y
        + tessellatedPosition[2] * gl_TessCoord.z;
    gl_Position = vec4(position, 0.0, 1.0);
}
