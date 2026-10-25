#pragma once

#include "dyf/Extends/Model/Model.h"

namespace dyf::Detail
{
struct SkinTransform
{
    Math::float4x4 matrix = {};
    bool copySource = true;
};

// Only influence/palette data enters the blend; source positions can share it.
// Zero total weight preserves the source exactly, including signed zero.
bool BuildSkinTransform(const SkinInfluence&, const std::vector<SkinJointMatrices>&,
    SkinTransform&, Math::float4x4* fallbackNormalMatrix = nullptr);
inline Math::float3 ApplySkinTransform(const SkinTransform& transform, const Math::float3& position)
{
    return transform.copySource ? position : Math::TransformPoint(transform.matrix, position);
}

// Compare complete influence bytes, preserving weight/joint order and DQ input.
bool BuildSkinTransformGroups(const std::vector<SkinInfluence>&,
    std::vector<uint32_t>& representatives, std::vector<uint32_t>& vertexGroups);

// Bounds need the same position blend as raster/compute, without evaluating
// inverse-transpose normals and orthogonalized tangents on the CPU again.
bool SkinPosition(const Vertex&, const SkinInfluence&,
    const std::vector<SkinJointMatrices>&, Math::float3&);
}
