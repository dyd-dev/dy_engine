#pragma once

#include <cstddef>
#include <cstdint>

#include "Math/Math.h"
#include "RHI/ShaderLayout.h"
#include "Graphics/RendererShaderLayout.inc"

namespace dy::Graphics::RendererShaderLayout
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
	constexpr uint32_t kBindlessMaterialStorageBinding = DY_RENDERER_BINDING_BINDLESS_MATERIAL_STORAGE;
	constexpr uint32_t kBindlessTransformStorageBinding = DY_RENDERER_BINDING_BINDLESS_TRANSFORM_STORAGE;
	constexpr uint32_t kBindlessDrawStorageBinding = DY_RENDERER_BINDING_BINDLESS_DRAW_STORAGE;
	constexpr uint32_t kSkinInfluenceStorageBinding = DY_RENDERER_BINDING_SKIN_INFLUENCE_STORAGE;
	constexpr uint32_t kSkinPaletteStorageBinding = DY_RENDERER_BINDING_SKIN_PALETTE_STORAGE;
	constexpr uint32_t kVulkanDrawConstantsBinding = DY_VULKAN_BINDING_DRAW_CONSTANTS;
	constexpr uint32_t kDescriptorBindingCount = DY_RENDERER_DESCRIPTOR_BINDING_COUNT;
	constexpr uint32_t kVulkanSkinningDescriptorBindingCount = DY_VULKAN_SKINNING_DESCRIPTOR_BINDING_COUNT;
	constexpr uint32_t kVulkanDescriptorBindingCount = DY_VULKAN_DESCRIPTOR_BINDING_COUNT;
	constexpr uint32_t kMaterialTextureBindingCount = DY_RENDERER_MATERIAL_TEXTURE_BINDING_COUNT;
	constexpr uint32_t kSamplerDescriptorCount = DY_RENDERER_SAMPLER_DESCRIPTOR_COUNT;
	constexpr uint32_t kConstantBufferDescriptorCount = DY_RENDERER_CONSTANT_BUFFER_DESCRIPTOR_COUNT;
	constexpr uint32_t kStorageBufferDescriptorCount = DY_RENDERER_STORAGE_BUFFER_DESCRIPTOR_COUNT;
	constexpr uint32_t kVulkanSkinningStorageBufferDescriptorCount = DY_VULKAN_SKINNING_STORAGE_BUFFER_DESCRIPTOR_COUNT;
	constexpr uint32_t kBindlessTextureCount = DY_RENDERER_BINDLESS_TEXTURE_COUNT;
	constexpr uint32_t kRendererVertexFloatCount = DY_RENDERER_VERTEX_FLOAT_COUNT;

	constexpr uint32_t kBaseColorTextureFlag = DY_RENDERER_TEXTURE_FLAG_BASE_COLOR;
	constexpr uint32_t kMetallicRoughnessTextureFlag = DY_RENDERER_TEXTURE_FLAG_METALLIC_ROUGHNESS;
	constexpr uint32_t kNormalTextureFlag = DY_RENDERER_TEXTURE_FLAG_NORMAL;
	constexpr uint32_t kOcclusionTextureFlag = DY_RENDERER_TEXTURE_FLAG_OCCLUSION;
	constexpr uint32_t kEmissiveTextureFlag = DY_RENDERER_TEXTURE_FLAG_EMISSIVE;
	constexpr uint32_t kReceiveShadowFlag = DY_RENDERER_TEXTURE_FLAG_RECEIVE_SHADOW;
	constexpr uint32_t kCastShadowFlag = DY_RENDERER_TEXTURE_FLAG_CAST_SHADOW;
	constexpr uint32_t kSkinnedFlag = DY_RENDERER_TEXTURE_FLAG_SKINNED;

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
		// emissiveColor.xyz = 방출색, emissiveColor.w = bindless 방출 텍스처 디스크립터 인덱스.
		Math::float4 emissiveColor = Math::float4(0.0f, 0.0f, 0.0f, 0.0f);
		Math::float4 baseColor;
		Math::float4 materialParams;
		// bindless 텍스처 디스크립터 인덱스: x=baseColor, y=metallicRoughness, z=normal, w=occlusion.
		// (방출은 emissiveColor.w). 인덱스는 0..127 정수라 float 에 정확히 표현됨. non-bindless 경로는 무시.
		Math::float4 textureIndices = Math::float4(0.0f, 0.0f, 0.0f, 0.0f);
	};

	constexpr uint32_t kDrawModePushConstantOffset = static_cast<uint32_t>(offsetof(DrawConstants, drawMode));
	constexpr uint32_t kDrawMetadataPushConstantOffset = static_cast<uint32_t>(offsetof(DrawConstants, firstIndex));
	constexpr uint32_t kMaterialConstantsPushConstantOffset = static_cast<uint32_t>(offsetof(DrawConstants, baseColor));
	constexpr uint32_t kPushConstantRangeSize = static_cast<uint32_t>(sizeof(DrawConstants));

	static_assert(kDrawModePushConstantOffset == 128u, "Renderer draw mode offset must match shader layout.");
	static_assert(kDrawMetadataPushConstantOffset == 132u, "Renderer draw metadata offset must match shader layout.");
	static_assert(kMaterialConstantsPushConstantOffset == 160u, "Renderer material constants offset must match shader layout.");
	static_assert(offsetof(DrawConstants, textureIndices) == 192u, "Renderer bindless texture indices offset must match shader layout.");
	static_assert(kPushConstantRangeSize == 208u, "Renderer draw constants must match push constant range.");
	static_assert(kBindlessDrawStorageBinding + 1u == kDescriptorBindingCount, "Renderer descriptor bindings must remain contiguous.");
	static_assert(kSkinInfluenceStorageBinding == kDescriptorBindingCount, "Vulkan skin influence binding must follow the shared layout.");
	static_assert(kSkinPaletteStorageBinding + 1u == kVulkanSkinningDescriptorBindingCount, "Vulkan skinning bindings must remain contiguous.");
	static_assert(kVulkanDrawConstantsBinding + 1u == kVulkanDescriptorBindingCount, "Vulkan draw constants binding must terminate the Vulkan layout.");
	static_assert(kMaterialTextureBindingCount + 1u == kSamplerDescriptorCount, "Renderer sampler descriptor count must include shadow map.");

	// RHI::ShaderLayoutDesc(데이터) 기본값이 셰이더 공유 .inc 계약과 일치함을 보증한다.
	// (둘 중 하나만 바뀌면 컴파일 실패 → 드리프트 방지)
	static_assert(RHI::ShaderLayoutDesc{}.baseColorTextureBinding == kBaseColorTextureBinding, "ShaderLayoutDesc drift: base color binding");
	static_assert(RHI::ShaderLayoutDesc{}.lightingConstantBinding == kLightingConstantBinding, "ShaderLayoutDesc drift: lighting binding");
	static_assert(RHI::ShaderLayoutDesc{}.shadowSamplerBinding == kShadowSamplerBinding, "ShaderLayoutDesc drift: shadow sampler binding");
	static_assert(RHI::ShaderLayoutDesc{}.shadowMatrixBinding == kShadowMatrixBinding, "ShaderLayoutDesc drift: shadow matrix binding");
	static_assert(RHI::ShaderLayoutDesc{}.vertexStorageBinding == kVertexStorageBinding, "ShaderLayoutDesc drift: vertex storage binding");
	static_assert(RHI::ShaderLayoutDesc{}.indexStorageBinding == kIndexStorageBinding, "ShaderLayoutDesc drift: index storage binding");
	static_assert(RHI::ShaderLayoutDesc{}.bindlessMaterialStorageBinding == kBindlessMaterialStorageBinding, "ShaderLayoutDesc drift: bindless material binding");
	static_assert(RHI::ShaderLayoutDesc{}.bindlessTransformStorageBinding == kBindlessTransformStorageBinding, "ShaderLayoutDesc drift: bindless transform binding");
	static_assert(RHI::ShaderLayoutDesc{}.bindlessDrawStorageBinding == kBindlessDrawStorageBinding, "ShaderLayoutDesc drift: bindless draw binding");
	static_assert(RHI::ShaderLayoutDesc{}.descriptorBindingCount == kDescriptorBindingCount, "ShaderLayoutDesc drift: descriptor binding count");
	static_assert(RHI::ShaderLayoutDesc{}.materialTextureBindingCount == kMaterialTextureBindingCount, "ShaderLayoutDesc drift: material texture count");
	static_assert(RHI::ShaderLayoutDesc{}.samplerDescriptorCount == kSamplerDescriptorCount, "ShaderLayoutDesc drift: sampler descriptor count");
	static_assert(RHI::ShaderLayoutDesc{}.constantBufferDescriptorCount == kConstantBufferDescriptorCount, "ShaderLayoutDesc drift: constant buffer descriptor count");
	static_assert(RHI::ShaderLayoutDesc{}.storageBufferDescriptorCount == kStorageBufferDescriptorCount, "ShaderLayoutDesc drift: storage buffer descriptor count");
	static_assert(RHI::ShaderLayoutDesc{}.bindlessTextureCount == kBindlessTextureCount, "ShaderLayoutDesc drift: bindless texture count");
	static_assert(RHI::ShaderLayoutDesc{}.pushConstantRangeSize == kPushConstantRangeSize, "ShaderLayoutDesc drift: push constant range size");
	static_assert(RHI::ShaderLayoutDesc{}.drawModePushConstantOffset == kDrawModePushConstantOffset, "ShaderLayoutDesc drift: draw mode offset");
	static_assert(RHI::ShaderLayoutDesc{}.drawMetadataPushConstantOffset == kDrawMetadataPushConstantOffset, "ShaderLayoutDesc drift: draw metadata offset");
	static_assert(RHI::ShaderLayoutDesc{}.castShadowFlag == kCastShadowFlag, "ShaderLayoutDesc drift: cast shadow flag");
}
