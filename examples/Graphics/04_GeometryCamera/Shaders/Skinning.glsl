#define float3 vec3
#define float4 vec4
#define float4x4 mat4
#define uint4 uvec4
#include "SkinningTypes.inc"
layout(std430, set=0, binding=RENDERER_BINDING_SKIN_INFLUENCES) readonly buffer InfluenceStorage { SkinInfluence values[]; } skinInfluences;
layout(std430, set=0, binding=RENDERER_BINDING_SKIN_PALETTE) readonly buffer PaletteStorage { SkinJointMatrices values[]; } skinPalette;
mat4 SkinColumns(vec4 a, vec4 b, vec4 c, vec4 d) { return mat4(a,b,c,d); }
vec4 SkinColumn(mat4 matrix, int index) { return matrix[index]; }
#define DY_SKIN_PARAMETERS
#define DY_SKIN_INLINE
#define DY_SKIN_OUT(type,name) out type name
#define DY_SKIN_INFLUENCE(index) skinInfluences.values[index]
#define DY_SKIN_JOINT(index) skinPalette.values[index]
#include "SkinningMath.inc"
#undef float3
#undef float4
#undef float4x4
#undef uint4
