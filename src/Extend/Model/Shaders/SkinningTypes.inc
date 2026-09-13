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
