#version 450
layout(local_size_x=1, local_size_y=1, local_size_z=1) in;
layout(push_constant) uniform Settings { vec4 settings; };
layout(set=0, binding=0, std430) buffer Arguments { uint words[]; };
void main()
{
    uint count = uint(floor(64.0 * clamp(0.5 + 0.75 * sin(settings.x * 0.65), 0.0, 1.0)));
    words[0] = 3u; words[1] = count; words[2] = 0u; words[3] = 0u;
    words[4] = 3u; words[5] = 64u - count; words[6] = 0u; words[7] = 0u; words[8] = 0u;
}
