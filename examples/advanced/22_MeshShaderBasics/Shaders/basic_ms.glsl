#version 450
#extension GL_EXT_mesh_shader : require

layout(local_size_x = 3, local_size_y = 1, local_size_z = 1) in;
layout(triangles, max_vertices = 3, max_primitives = 1) out;

layout(location = 0) out vec4 outColor[];

const vec2 positions[3] = vec2[](
    vec2( 0.0,  0.5),
    vec2( 0.5, -0.5),
    vec2(-0.5, -0.5)
);

const vec3 colors[3] = vec3[](
    vec3(1.0, 0.0, 0.0),
    vec3(0.0, 1.0, 0.0),
    vec3(0.0, 0.0, 1.0)
);

void main()
{
    uint gtid = gl_LocalInvocationIndex;
    SetMeshOutputsEXT(3, 1);

    gl_MeshVerticesEXT[gtid].gl_Position = vec4(positions[gtid], 0.0, 1.0);
    outColor[gtid] = vec4(colors[gtid], 1.0);

    if (gtid == 0) {
        gl_PrimitiveTriangleIndicesEXT[0] = uvec3(0, 1, 2);
    }
}
