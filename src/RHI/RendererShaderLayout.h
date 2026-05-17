#pragma once

#include <cstddef>
#include <cstdint>

#include "Math/Math.h"
#include "RHI/RendererShaderLayout.inc"

namespace dy::RHI::RendererShaderLayout
{
	constexpr uint32_t kBaseColorTextureBinding = DY_RENDERER_BINDING_BASE_COLOR_TEXTURE;
	constexpr uint32_t kLightingConstantBinding = DY_RENDERER_BINDING_LIGHTING_CONSTANTS;
	constexpr uint32_t kShadowSamplerBinding = DY_RENDERER_BINDING_SHADOW_MAP;
	constexpr uint32_t kShadowMatrixBinding = DY_RENDERER_BINDING_SHADOW_MATRIX;
	constexpr uint32_t kVertexStorageBinding = DY_RENDERER_BINDING_VERTEX_STORAGE;
	constexpr uint32_t kIndexStorageBinding = DY_RENDERER_BINDING_INDEX_STORAGE;
	constexpr uint32_t kMetallicRoughnessSamplerBinding = DY_RENDERER_BINDING_METALLIC_ROUGHNESS_TEXTURE;
	constexpr uint32_t kNormalSamplerBinding = DY_RENDERER_BINDING_NORMAL_TEXTURE;
	constexpr uint32_t kOcclusionSamplerBinding = DY_RENDERER_BINDING_OCCLUSION_TEXTURE;
	constexpr uint32_t kEmissiveSamplerBinding = DY_RENDERER_BINDING_EMISSIVE_TEXTURE;
	constexpr uint32_t kDescriptorBindingCount = DY_RENDERER_DESCRIPTOR_BINDING_COUNT;
	constexpr uint32_t kMaterialTextureBindingCount = DY_RENDERER_MATERIAL_TEXTURE_BINDING_COUNT;
	constexpr uint32_t kSamplerDescriptorCount = DY_RENDERER_SAMPLER_DESCRIPTOR_COUNT;
	constexpr uint32_t kConstantBufferDescriptorCount = DY_RENDERER_CONSTANT_BUFFER_DESCRIPTOR_COUNT;
	constexpr uint32_t kStorageBufferDescriptorCount = DY_RENDERER_STORAGE_BUFFER_DESCRIPTOR_COUNT;
	constexpr uint32_t kRendererVertexFloatCount = DY_RENDERER_VERTEX_FLOAT_COUNT;

	constexpr uint32_t kBaseColorTextureFlag = DY_RENDERER_TEXTURE_FLAG_BASE_COLOR;
	constexpr uint32_t kMetallicRoughnessTextureFlag = DY_RENDERER_TEXTURE_FLAG_METALLIC_ROUGHNESS;
	constexpr uint32_t kNormalTextureFlag = DY_RENDERER_TEXTURE_FLAG_NORMAL;
	constexpr uint32_t kOcclusionTextureFlag = DY_RENDERER_TEXTURE_FLAG_OCCLUSION;
	constexpr uint32_t kEmissiveTextureFlag = DY_RENDERER_TEXTURE_FLAG_EMISSIVE;
	constexpr uint32_t kReceiveShadowFlag = DY_RENDERER_TEXTURE_FLAG_RECEIVE_SHADOW;
	constexpr uint32_t kCastShadowFlag = DY_RENDERER_TEXTURE_FLAG_CAST_SHADOW;

	struct RendererLightingConstants
	{
		Math::float4 cameraPosition;
		Math::float4 directionalLightDirection;
		Math::float4 directionalLightColor;
		Math::float4 ambientColor;
		Math::float4 shadowParams;
		Math::float4 pbrParams;
		Math::float4 environmentColor;
		Math::float4 pointLightPositionRange;
		Math::float4 pointLightColorIntensity;
	};

	struct RendererShadowConstants
	{
		Math::float4x4 lightViewProjectionMatrix;
	};

	struct DrawConstants
	{
		Math::float4x4 viewProjectionMatrix;
		Math::float4x4 modelMatrix;
		float drawMode = 0.0f;
		uint32_t firstIndex = 0;
		int32_t vertexOffset = 0;
		uint32_t firstVertex = 0;
		Math::float3 emissiveColor = Math::float3(0.0f, 0.0f, 0.0f);
		Math::float4 baseColor;
		Math::float4 materialParams;
	};

	constexpr uint32_t kDrawModePushConstantOffset = static_cast<uint32_t>(offsetof(DrawConstants, drawMode));
	constexpr uint32_t kDrawMetadataPushConstantOffset = static_cast<uint32_t>(offsetof(DrawConstants, firstIndex));
	constexpr uint32_t kMaterialConstantsPushConstantOffset = static_cast<uint32_t>(offsetof(DrawConstants, baseColor));
	constexpr uint32_t kPushConstantRangeSize = static_cast<uint32_t>(sizeof(DrawConstants));

	static_assert(kDrawModePushConstantOffset == 128u, "Renderer draw mode offset must match shader layout.");
	static_assert(kDrawMetadataPushConstantOffset == 132u, "Renderer draw metadata offset must match shader layout.");
	static_assert(kMaterialConstantsPushConstantOffset == 160u, "Renderer material constants offset must match shader layout.");
	static_assert(kPushConstantRangeSize == 192u, "Renderer draw constants must match push constant range.");
	static_assert(kEmissiveSamplerBinding + 1u == kDescriptorBindingCount, "Renderer descriptor bindings must remain contiguous.");
	static_assert(kMaterialTextureBindingCount + 1u == kSamplerDescriptorCount, "Renderer sampler descriptor count must include shadow map.");
}
