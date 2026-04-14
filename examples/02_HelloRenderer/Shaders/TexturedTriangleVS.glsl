#version 450

layout(push_constant) uniform DrawConstants
{
    mat4 worldMatrix;
    vec4 baseColor;
    uint baseColorTextureIndex;
} gDraw;

layout(location = 0) out vec2 outUV;

void main()
{
    const vec2 positions[3] = vec2[3](
        vec2(0.0, 0.6),
        vec2(0.6, -0.6),
        vec2(-0.6, -0.6)
    );

    const vec2 uvs[3] = vec2[3](
        vec2(0.5, 0.0),
        vec2(1.0, 1.0),
        vec2(0.0, 1.0)
    );

    gl_Position = gDraw.worldMatrix * vec4(positions[gl_VertexIndex], 0.0, 1.0);
    outUV = uvs[gl_VertexIndex];
}
