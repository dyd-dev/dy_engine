#pragma once

#include "ShaderLayout.h"
#include "dyf/Material.h"
#include "dyf/RHI/IDevice.h"
#include "dyf/RHI/ICommandList.h"
#include <cstring>

namespace dyf
{
inline bool RecordBufferUpload(RHI::IDevice* device, RHI::ICommandList& commands,
		RHI::BufferHandle buffer, const void* data, uint32_t sizeBytes, RHI::ResourceState useState)
	{
		if(device == nullptr || buffer == nullptr || data == nullptr || sizeBytes == 0) return false;
		if(!device->UpdateBuffer(commands, buffer, 0, data, sizeBytes)) return false;
		const RHI::ResourceBarrierDesc ready = {
			buffer, nullptr, RHI::ResourceState::CopyDestination, useState, {}
		};
		commands.ResourceBarrier(&ready, 1);
		return true;
	}

inline DrawConstants MakeDrawConstants(const Math::float4x4& viewProjection,
		const MaterialDesc& material, const Math::float4x4& transform, uint32_t textureFlags)
	{
		DrawConstants constants = {};
		constants.viewProjectionMatrix = viewProjection;
		constants.modelMatrix = transform;
		constants.textureFlags = textureFlags;
		static_assert(sizeof(constants.padding0) == sizeof(material.alphaCutoff));
		std::memcpy(&constants.padding0, &material.alphaCutoff, sizeof(constants.padding0));
		constants.padding1 = static_cast<uint32_t>(material.alphaMode) | (material.doubleSided ? 4u : 0u) |
			(material.normalYSign < 0 ? 8u : 0u) | (material.unlit ? 16u : 0u);
		constants.emissiveColor = {material.emissiveColor.x, material.emissiveColor.y, material.emissiveColor.z, 0};
		constants.baseColor = material.baseColor;
		constants.materialParams = {material.metallicFactor, material.roughnessFactor,
			material.normalScale, material.occlusionStrength};
		return constants;
	}
}
