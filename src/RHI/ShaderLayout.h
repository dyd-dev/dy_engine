#pragma once
#include <cstdint>
#include "Format.h"

namespace dy::RHI
{
	struct ShaderLayoutDesc
	{
		// 디스크립터 바인딩 인덱스 (셰이더의 layout(binding=...) 과 일치)
		BindingIndex baseColorTextureBinding = 0;
		BindingIndex lightingConstantBinding = 1;
		BindingIndex shadowSamplerBinding = 2;
		BindingIndex shadowMatrixBinding = 3;
		BindingIndex vertexStorageBinding = 4;
		BindingIndex indexStorageBinding = 5;
		BindingIndex metallicRoughnessTextureBinding = 6;
		BindingIndex normalTextureBinding = 7;
		BindingIndex occlusionTextureBinding = 8;
		BindingIndex emissiveTextureBinding = 9;
		BindingIndex bindlessMaterialStorageBinding = 10;
		BindingIndex bindlessTransformStorageBinding = 11;
		BindingIndex bindlessDrawStorageBinding = 12;
		uint32_t descriptorBindingCount = 13;

		// 디스크립터 풀 크기 산정용 개수
		uint32_t materialTextureBindingCount = 5;
		uint32_t samplerDescriptorCount = 6;
		uint32_t constantBufferDescriptorCount = 2;
		uint32_t storageBufferDescriptorCount = 5;
		uint32_t bindlessTextureCount = 128;

		// 푸시 상수 범위/오프셋 (바이트)
		// 208 = 192(기존) + 16(bindless 텍스처 디스크립터 인덱스용 float4 textureIndices)
		uint32_t pushConstantRangeSize = 208;
		uint32_t drawModePushConstantOffset = 128;
		uint32_t drawMetadataPushConstantOffset = 132;

		// 백엔드가 푸시상수에서 그림자 플래그를 읽을 때 사용
		uint32_t castShadowFlag = 64;
	};
}
