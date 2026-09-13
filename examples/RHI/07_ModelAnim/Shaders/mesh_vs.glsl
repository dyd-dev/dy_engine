#version 450

struct SkinInfluence {
    uvec4 joints;
    vec4 weights;
    float dqBlendWeight;
    float padding0;
    float padding1;
    float padding2;
};
struct SkinJointMatrices {
    mat4 positionMatrix;
    mat4 normalMatrix;
    vec4 dqReal;
    vec4 dqDual;
    vec4 dqScale;
};

layout(std430, set=0, binding=11) readonly buffer InfluenceStorage { SkinInfluence values[]; } skinInfluences;
layout(std430, set=0, binding=12) readonly buffer PaletteStorage { SkinJointMatrices values[]; } skinPalette;
mat4 SkinColumns(vec4 a, vec4 b, vec4 c, vec4 d) { return mat4(a,b,c,d); }
vec4 SkinColumn(mat4 matrix, int index) { return matrix[index]; }
// 모델의 LBS·강체·DQ·혼합 DQ 스키닝을 현재 셰이더 언어로 계산한다.
mat4 SkinIdentity() {
    return SkinColumns(vec4(1,0,0,0), vec4(0,1,0,0), vec4(0,0,1,0), vec4(0,0,0,1));
}

mat4 SkinNormalMatrix(mat4 matrix, mat4 fallback) {
    vec3 a = SkinColumn(matrix, 0).xyz;
    vec3 b = SkinColumn(matrix, 1).xyz;
    vec3 c = SkinColumn(matrix, 2).xyz;
    float determinant = dot(a, cross(b,c));
    if (abs(determinant) <= 0.00000001) return fallback;
    return SkinColumns(vec4(cross(b,c)/determinant,0), vec4(cross(c,a)/determinant,0),
        vec4(cross(a,b)/determinant,0), vec4(0,0,0,1));
}

vec4 QuaternionProduct(vec4 a, vec4 b) {
    return vec4(a.w*b.xyz + b.w*a.xyz + cross(a.xyz,b.xyz), a.w*b.w - dot(a.xyz,b.xyz));
}

mat4 DualQuaternionMatrix(vec4 real, vec4 dual, vec3 scale) {
    float lengthReal = length(real);
    real /= lengthReal;
    dual /= lengthReal;
    vec3 translation = 2.0 * QuaternionProduct(dual, vec4(-real.xyz,real.w)).xyz;
    float x=real.x, y=real.y, z=real.z, w=real.w;
    return SkinColumns(
        vec4(vec3(1-2*(y*y+z*z), 2*(x*y+w*z), 2*(x*z-w*y))*scale.x,0),
        vec4(vec3(2*(x*y-w*z), 1-2*(x*x+z*z), 2*(y*z+w*x))*scale.y,0),
        vec4(vec3(2*(x*z+w*y), 2*(y*z-w*x), 1-2*(x*x+y*y))*scale.z,0),
        vec4(translation,1));
}

 void LoadSkinning( uint vertexIndex, uint influenceOffset, uint paletteOffset,
    out mat4 matrix, out mat4 normalMatrix) {
    matrix = SkinIdentity();
    normalMatrix = SkinIdentity();
    if (influenceOffset == 0xffffffffu || paletteOffset == 0xffffffffu) return;
    SkinInfluence influence = skinInfluences.values[influenceOffset + vertexIndex];
    float total = dot(influence.weights, vec4(1,1,1,1));
    if (total <= 0.000001) return;
    vec4 weights = influence.weights / total;
    mat4 linearSkin = SkinIdentity() * 0.0;
    mat4 normalFallback = SkinIdentity() * 0.0;
    vec4 reference = vec4(0,0,0,1);
    vec4 real = vec4(0,0,0,0), dual = vec4(0,0,0,0);
    vec3 scale = vec3(0,0,0);
    bool haveReference = false;
    for (uint component=0u; component<4u; ++component) {
        float weight = weights[component];
        if (weight <= 0.0) continue;
        SkinJointMatrices joint = skinPalette.values[paletteOffset + influence.joints[component]];
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
        mat4 dqMatrix = DualQuaternionMatrix(real,dual,scale);
        matrix += (dqMatrix-linearSkin) * clamp(influence.dqBlendWeight,0.0,1.0);
    }
    normalMatrix = SkinNormalMatrix(matrix,normalFallback);
}

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUv;
layout(location = 3) in vec4 inTangent;

layout(location = 0) out vec2 fragUv;
layout(location = 1) out vec3 fragWorldPosition;
layout(location = 2) out vec3 fragNormal;
layout(location = 3) out vec4 fragTangent;

layout(push_constant) uniform DrawConstants
{
    mat4 viewProjectionMatrix;
    mat4 modelMatrix;
    uint textureFlags;
    uint padding0;
    uint padding1;
    uint padding2;
    vec4 emissiveColor;
    vec4 baseColor;
    vec4 materialParams;
} drawConstants;

void main()
{
    mat4 skin, skinNormal;
    LoadSkinning(gl_VertexIndex, drawConstants.padding0, drawConstants.padding1, skin, skinNormal);
    mat4 world = drawConstants.modelMatrix * skin;
    mat4 normalMatrix = SkinNormalMatrix(world, skinNormal);
    vec4 worldPosition = world * vec4(inPosition, 1.0);
    gl_Position = drawConstants.viewProjectionMatrix * worldPosition;
    fragUv = inUv;
    fragWorldPosition = worldPosition.xyz;
    fragNormal = normalize(mat3(normalMatrix) * inNormal);
    vec3 tangent = mat3(world) * inTangent.xyz;
    tangent = normalize(tangent - fragNormal * dot(fragNormal, tangent));
    fragTangent = vec4(tangent, determinant(mat3(world)) < 0.0 ? -inTangent.w : inTangent.w);
}
