// Shared LBS / rigid / DQ / blended-DQ algebra. Language wrappers only define
// matrix column access, buffer access and output-parameter syntax.
DY_SKIN_INLINE float4x4 SkinIdentity() {
    return SkinColumns(float4(1,0,0,0), float4(0,1,0,0), float4(0,0,1,0), float4(0,0,0,1));
}

DY_SKIN_INLINE float4x4 SkinNormalMatrix(float4x4 matrix, float4x4 fallback) {
    float3 a = SkinColumn(matrix, 0).xyz;
    float3 b = SkinColumn(matrix, 1).xyz;
    float3 c = SkinColumn(matrix, 2).xyz;
    float determinant = dot(a, cross(b,c));
    if (abs(determinant) <= 0.00000001) return fallback;
    return SkinColumns(float4(cross(b,c)/determinant,0), float4(cross(c,a)/determinant,0),
        float4(cross(a,b)/determinant,0), float4(0,0,0,1));
}

DY_SKIN_INLINE float4 QuaternionProduct(float4 a, float4 b) {
    return float4(a.w*b.xyz + b.w*a.xyz + cross(a.xyz,b.xyz), a.w*b.w - dot(a.xyz,b.xyz));
}

DY_SKIN_INLINE float4x4 DualQuaternionMatrix(float4 real, float4 dual, float3 scale) {
    float lengthReal = length(real);
    real /= lengthReal;
    dual /= lengthReal;
    float3 translation = 2.0 * QuaternionProduct(dual, float4(-real.xyz,real.w)).xyz;
    float x=real.x, y=real.y, z=real.z, w=real.w;
    return SkinColumns(
        float4(float3(1-2*(y*y+z*z), 2*(x*y+w*z), 2*(x*z-w*y))*scale.x,0),
        float4(float3(2*(x*y-w*z), 1-2*(x*x+z*z), 2*(y*z+w*x))*scale.y,0),
        float4(float3(2*(x*z+w*y), 2*(y*z-w*x), 1-2*(x*x+y*y))*scale.z,0),
        float4(translation,1));
}

DY_SKIN_INLINE void LoadSkinning(DY_SKIN_PARAMETERS uint vertexIndex, uint influenceOffset, uint paletteOffset,
    DY_SKIN_OUT(float4x4, matrix), DY_SKIN_OUT(float4x4, normalMatrix)) {
    matrix = SkinIdentity();
    normalMatrix = SkinIdentity();
    if (influenceOffset == 0xffffffffu || paletteOffset == 0xffffffffu) return;
    SkinInfluence influence = DY_SKIN_INFLUENCE(influenceOffset + vertexIndex);
    float total = dot(influence.weights, float4(1,1,1,1));
    if (total <= 0.000001) return;
    float4 weights = influence.weights / total;
    float4x4 linear = SkinIdentity() * 0.0;
    float4x4 normalFallback = SkinIdentity() * 0.0;
    float4 reference = float4(0,0,0,1);
    float4 real = float4(0,0,0,0), dual = float4(0,0,0,0);
    float3 scale = float3(0,0,0);
    bool haveReference = false;
    for (uint component=0u; component<4u; ++component) {
        float weight = weights[component];
        if (weight <= 0.0) continue;
        SkinJointMatrices joint = DY_SKIN_JOINT(paletteOffset + influence.joints[component]);
        linear += joint.positionMatrix * weight;
        normalFallback += joint.normalMatrix * weight;
        if (influence.dqBlendWeight > 0.0) {
            if (!haveReference) { reference=joint.dqReal; haveReference=true; }
            float signCorrection = dot(reference,joint.dqReal)<0.0 ? -1.0 : 1.0;
            real += joint.dqReal * (weight*signCorrection);
            dual += joint.dqDual * (weight*signCorrection);
            scale += joint.dqScale.xyz * weight;
        }
    }
    matrix = linear;
    if (influence.dqBlendWeight > 0.0 && length(real) > 0.000001) {
        float4x4 dqMatrix = DualQuaternionMatrix(real,dual,scale);
        matrix += (dqMatrix-linear) * clamp(influence.dqBlendWeight,0.0,1.0);
    }
    normalMatrix = SkinNormalMatrix(matrix,normalFallback);
}
