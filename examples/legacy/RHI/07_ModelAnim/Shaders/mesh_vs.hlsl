struct SkinInfluence {
    uint4 joints;
    float4 weights;
    float dqBlendWeight;
    float padding0;
    float padding1;
    float padding2;
};
struct SkinJointMatrices {
    float4x4 positionMatrix;
    float4x4 normalMatrix;
    float4 dqReal;
    float4 dqDual;
    float4 dqScale;
};

StructuredBuffer<SkinInfluence> skinInfluences : register(t11, space0);
StructuredBuffer<SkinJointMatrices> skinPalette : register(t12, space0);
float4x4 SkinColumns(float4 a, float4 b, float4 c, float4 d) { return transpose(float4x4(a,b,c,d)); }
float4 SkinColumn(float4x4 matrix, int index) { return float4(matrix[0][index],matrix[1][index],matrix[2][index],matrix[3][index]); }
// 모델의 LBS·강체·DQ·혼합 DQ 스키닝을 현재 셰이더 언어로 계산한다.
inline float4x4 SkinIdentity() {
    return SkinColumns(float4(1,0,0,0), float4(0,1,0,0), float4(0,0,1,0), float4(0,0,0,1));
}

inline float4x4 SkinNormalMatrix(float4x4 matrix, float4x4 fallback) {
    float3 a = SkinColumn(matrix, 0).xyz;
    float3 b = SkinColumn(matrix, 1).xyz;
    float3 c = SkinColumn(matrix, 2).xyz;
    float determinant = dot(a, cross(b,c));
    if (abs(determinant) <= 0.00000001) return fallback;
    return SkinColumns(float4(cross(b,c)/determinant,0), float4(cross(c,a)/determinant,0),
        float4(cross(a,b)/determinant,0), float4(0,0,0,1));
}

inline float4 QuaternionProduct(float4 a, float4 b) {
    return float4(a.w*b.xyz + b.w*a.xyz + cross(a.xyz,b.xyz), a.w*b.w - dot(a.xyz,b.xyz));
}

inline float4x4 DualQuaternionMatrix(float4 real, float4 dual, float3 scale) {
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

inline void LoadSkinning( uint vertexIndex, uint influenceOffset, uint paletteOffset,
    out float4x4 matrix, out float4x4 normalMatrix) {
    matrix = SkinIdentity();
    normalMatrix = SkinIdentity();
    if (influenceOffset == 0xffffffffu || paletteOffset == 0xffffffffu) return;
    SkinInfluence influence = skinInfluences[influenceOffset + vertexIndex];
    float total = dot(influence.weights, float4(1,1,1,1));
    if (total <= 0.000001) return;
    float4 weights = influence.weights / total;
    float4x4 linearSkin = SkinIdentity() * 0.0;
    float4x4 normalFallback = SkinIdentity() * 0.0;
    float4 reference = float4(0,0,0,1);
    float4 real = float4(0,0,0,0), dual = float4(0,0,0,0);
    float3 scale = float3(0,0,0);
    bool haveReference = false;
    for (uint component=0u; component<4u; ++component) {
        float weight = weights[component];
        if (weight <= 0.0) continue;
        SkinJointMatrices joint = skinPalette[paletteOffset + influence.joints[component]];
        linearSkin += joint.positionMatrix * weight;
        normalFallback += joint.normalMatrix * weight;
        if (influence.dqBlendWeight > 0.0) {
            if (!haveReference) { reference=joint.dqReal; haveReference=true; }
            float signCorrection = dot(reference,joint.dqReal)<0.0 ? -1.0 : 1.0;
            real += joint.dqReal * (weight*signCorrection);
            dual += joint.dqDual * (weight*signCorrection);
            scale += joint.dqScale.xyz * weight;
        }
    }
    matrix = linearSkin;
    if (influence.dqBlendWeight > 0.0 && length(real) > 0.000001) {
        float4x4 dqMatrix = DualQuaternionMatrix(real,dual,scale);
        matrix += (dqMatrix-linearSkin) * clamp(influence.dqBlendWeight,0.0,1.0);
    }
    normalMatrix = SkinNormalMatrix(matrix,normalFallback);
}

cbuffer DrawConstants : register(
    b10,
    space0)
{
    column_major float4x4 viewProjectionMatrix;
    column_major float4x4 modelMatrix;
    uint textureFlags;
    uint padding0;
    uint padding1;
    uint padding2;
    float4 emissiveColor;
    float4 baseColor;
    float4 materialParams;
};

struct VSInput
{
    float3 position : TEXCOORD0;
    float3 normal : TEXCOORD1;
    float2 uv : TEXCOORD2;
    float4 tangent : TEXCOORD3;
};

struct VSOutput
{
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
    float3 worldPosition : TEXCOORD1;
    float3 worldNormal : TEXCOORD2;
    float4 worldTangent : TEXCOORD3;
};

VSOutput main(VSInput input, uint vertexId : SV_VertexID)
{
    float4x4 skin, skinNormal;
    LoadSkinning(vertexId, padding0, padding1, skin, skinNormal);
    const float4x4 world = mul(modelMatrix, skin);
    const float4 worldPosition = mul(world, float4(input.position, 1.0));
    const float3x3 normalMatrix = (float3x3)SkinNormalMatrix(world, skinNormal);

    VSOutput output;
    output.position = mul(viewProjectionMatrix, worldPosition);
    output.uv = input.uv;
    output.worldPosition = worldPosition.xyz;
    output.worldNormal = normalize(mul(normalMatrix, input.normal));
    float3 tangent = mul((float3x3)world, input.tangent.xyz);
    tangent = normalize(tangent - output.worldNormal * dot(output.worldNormal, tangent));
    output.worldTangent = float4(tangent, determinant((float3x3)world) < 0.0 ? -input.tangent.w : input.tangent.w);
    return output;
}
