#pragma once

#include "dyf/Extends/Model/Animation.h"

namespace dyf::Detail
{
// Internal fast path: this exact clip must already have sampled successfully in
// the current call, or belong to an immutable ModelScene asset validated earlier.
// Per-instance indices, sample time and evaluated results are still checked.
bool SampleAnimationClipValidatedKeys(const AnimationClip&,float,
    std::vector<NodeTransform>&,std::vector<std::vector<float>>&);

struct NodeHierarchyScratch
{
    std::vector<uint8_t> visitState;
    std::vector<uint32_t> stack,order;
};

// Reuse temporary storage while retaining full hierarchy validation. Capturing
// the successful evaluation order lets immutable assets retain their topology.
bool BuildGlobalNodeMatricesWithScratch(const std::vector<ModelNode>&,
    const std::vector<NodeTransform>&,std::vector<Math::float4x4>&,
    NodeHierarchyScratch&,bool captureOrder);

// The order must come from a successful validation of these exact nodes in the
// current call, or the same immutable asset. Local/global poses remain instance-owned.
bool BuildGlobalNodeMatricesValidatedOrder(const std::vector<ModelNode>&,
    const std::vector<NodeTransform>&,std::vector<Math::float4x4>&,
    const std::vector<uint32_t>&);
}
