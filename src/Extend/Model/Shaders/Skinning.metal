#include "SkinningTypes.inc"
inline float4x4 SkinColumns(float4 a, float4 b, float4 c, float4 d) { return float4x4(a,b,c,d); }
inline float4 SkinColumn(float4x4 matrix, int index) { return matrix[index]; }
#define DY_SKIN_INLINE inline
#define DY_SKIN_PARAMETERS const device SkinInfluence* skinInfluences, const device SkinJointMatrices* skinPalette,
#define DY_SKIN_OUT(type,name) thread type& name
#define DY_SKIN_INFLUENCE(index) skinInfluences[index]
#define DY_SKIN_JOINT(index) skinPalette[index]
#include "SkinningMath.inc"
