cbuffer Settings : register(b15) { float4 settings; };
RWStructuredBuffer<uint> words : register(u0);
[numthreads(1, 1, 1)]
void main()
{
    uint count = uint(floor(64.0 * saturate(0.5 + 0.75 * sin(settings.x * 0.65))));
    words[0] = 3; words[1] = count; words[2] = 0; words[3] = 0;
    words[4] = 3; words[5] = 64 - count; words[6] = 0; words[7] = 0; words[8] = 0;
}
