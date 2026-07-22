#pragma once

#include <cstddef>
#include <cstdint>

#include "Math/Math.h"
#include "Graphics/RendererShaderLayout.inc"

namespace dy::Graphics::RendererShaderLayout
{
	constexpr uint32_t kBaseColorTextureBinding = DY_RENDERER_BINDING_BASE_COLOR_TEXTURE;
	constexpr uint32_t kLightingConstantBinding = DY_RENDERER_BINDING_LIGHTING_CONSTANTS;
	constexpr uint32_t kShadowSamplerBinding = DY_RENDERER_BINDING_SHADOW_MAP;
	constexpr uint32_t kShadowMatrixBinding = DY_RENDERER_BINDING_SHADOW_MATRIX;
	constexpr uint32_t kMetallicRoughnessSamplerBinding = DY_RENDERER_BINDING_METALLIC_ROUGHNESS_TEXTURE;
	constexpr uint32_t kNormalSamplerBinding = DY_RENDERER_BINDING_NORMAL_TEXTURE;
	constexpr uint32_t kOcclusionSamplerBinding = DY_RENDERER_BINDING_OCCLUSION_TEXTURE;
	constexpr uint32_t kEmissiveSamplerBinding = DY_RENDERER_BINDING_EMISSIVE_TEXTURE;
	constexpr uint32_t kBindlessMaterialStorageBinding = DY_RENDERER_BINDING_BINDLESS_MATERIAL_STORAGE;
	constexpr uint32_t kBindlessTransformStorageBinding = DY_RENDERER_BINDING_BINDLESS_TRANSFORM_STORAGE;
	constexpr uint32_t kBindlessDrawStorageBinding = DY_RENDERER_BINDING_BINDLESS_DRAW_STORAGE;
	constexpr uint32_t kDescriptorBindingCount = DY_RENDERER_DESCRIPTOR_BINDING_COUNT;
	constexpr uint32_t kMaterialTextureBindingCount = DY_RENDERER_MATERIAL_TEXTURE_BINDING_COUNT;
	constexpr uint32_t kSamplerDescriptorCount = DY_RENDERER_SAMPLER_DESCRIPTOR_COUNT;
	constexpr uint32_t kConstantBufferDescriptorCount = DY_RENDERER_CONSTANT_BUFFER_DESCRIPTOR_COUNT;
	constexpr uint32_t kStorageBufferDescriptorCount = DY_RENDERER_STORAGE_BUFFER_DESCRIPTOR_COUNT;
	constexpr uint32_t kBindlessTextureCount = DY_RENDERER_BINDLESS_TEXTURE_COUNT;

	constexpr uint32_t kBaseColorTextureFlag = DY_RENDERER_TEXTURE_FLAG_BASE_COLOR;
	constexpr uint32_t kMetallicRoughnessTextureFlag = DY_RENDERER_TEXTURE_FLAG_METALLIC_ROUGHNESS;
	constexpr uint32_t kNormalTextureFlag = DY_RENDERER_TEXTURE_FLAG_NORMAL;
	constexpr uint32_t kOcclusionTextureFlag = DY_RENDERER_TEXTURE_FLAG_OCCLUSION;
	constexpr uint32_t kEmissiveTextureFlag = DY_RENDERER_TEXTURE_FLAG_EMISSIVE;
	constexpr uint32_t kReceiveShadowFlag = DY_RENDERER_TEXTURE_FLAG_RECEIVE_SHADOW;
	constexpr uint32_t kCastShadowFlag = DY_RENDERER_TEXTURE_FLAG_CAST_SHADOW;

	struct RendererVertex
	{
		float px = 0.0f, py = 0.0f, pz = 0.0f;
		float nx = 0.0f, ny = 0.0f, nz = 1.0f;
		float u = 0.0f, v = 0.0f;
		float tx = 1.0f, ty = 0.0f, tz = 0.0f, tw = 1.0f;
	};
	static_assert(sizeof(RendererVertex) == sizeof(float) * 12u, "Renderer vertex layout must match stock shaders.");

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
		uint32_t instanceTransformOffset = 0;
		// emissiveColor.xyz = 방출색, emissiveColor.w = bindless 방출 텍스처 디스크립터 인덱스.
		Math::float4 emissiveColor = Math::float4(0.0f, 0.0f, 0.0f, 0.0f);
		Math::float4 baseColor;
		Math::float4 materialParams;
		// bindless 텍스처 디스크립터 인덱스: x=baseColor, y=metallicRoughness, z=normal, w=occlusion.
		// (방출은 emissiveColor.w). 인덱스는 0..127 정수라 float 에 정확히 표현됨. non-bindless 경로는 무시.
		Math::float4 textureIndices = Math::float4(0.0f, 0.0f, 0.0f, 0.0f);
	};

	constexpr uint32_t kDrawModePushConstantOffset = static_cast<uint32_t>(offsetof(DrawConstants, drawMode));
	constexpr uint32_t kInstanceTransformPushConstantOffset = static_cast<uint32_t>(offsetof(DrawConstants, instanceTransformOffset));
	constexpr uint32_t kMaterialConstantsPushConstantOffset = static_cast<uint32_t>(offsetof(DrawConstants, baseColor));
	constexpr uint32_t kPushConstantRangeSize = static_cast<uint32_t>(sizeof(DrawConstants));

	static_assert(kDrawModePushConstantOffset == 128u, "Renderer draw mode offset must match shader layout.");
	static_assert(kInstanceTransformPushConstantOffset == 132u, "Renderer instance transform offset must match shader layout.");
	static_assert(kMaterialConstantsPushConstantOffset == 160u, "Renderer material constants offset must match shader layout.");
	static_assert(offsetof(DrawConstants, textureIndices) == 192u, "Renderer bindless texture indices offset must match shader layout.");
	static_assert(kPushConstantRangeSize == 208u, "Renderer draw constants must match push constant range.");
	static_assert(kMaterialTextureBindingCount + 1u == kSamplerDescriptorCount, "Renderer sampler descriptor count must include shadow map.");

}
