#include "SkinningTypes.inc"
StructuredBuffer<SkinInfluence> skinInfluences : register(t11, space0);
StructuredBuffer<SkinJointMatrices> skinPalette : register(t12, space0);
float4x4 SkinColumns(float4 a, float4 b, float4 c, float4 d) { return transpose(float4x4(a,b,c,d)); }
float4 SkinColumn(float4x4 matrix, int index) { return float4(matrix[0][index],matrix[1][index],matrix[2][index],matrix[3][index]); }
#define DY_SKIN_PARAMETERS
#define DY_SKIN_INLINE inline
#define DY_SKIN_OUT(type,name) out type name
#define DY_SKIN_INFLUENCE(index) skinInfluences[index]
#define DY_SKIN_JOINT(index) skinPalette[index]
#include "SkinningMath.inc"
