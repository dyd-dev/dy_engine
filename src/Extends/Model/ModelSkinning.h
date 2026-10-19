#pragma once

#include "dyf/Extends/Model/Model.h"

namespace dyf::Detail
{
// Bounds need the same position blend as raster/compute, without evaluating
// inverse-transpose normals and orthogonalized tangents on the CPU again.
bool SkinPosition(const Vertex&, const SkinInfluence&,
    const std::vector<SkinJointMatrices>&, Math::float3&);
}
