#version 450
layout(set = 0, binding = 4, std430) readonly buffer Vertices { float data[]; } vertices;
layout(set = 0, binding = 15, std140) uniform Constants { vec4 color; vec4 parameters; } constants;
layout(location = 0) out vec2 uv;
void main()
{
    uint offset = uint(gl_VertexIndex) * 12;
    gl_Position = vec4(vertices.data[offset] * constants.parameters.x,
                       vertices.data[offset + 1] * constants.parameters.x * constants.parameters.z,
                       constants.parameters.y, 1.0);
    uv = vec2(vertices.data[offset + 6], vertices.data[offset + 7]);
}
