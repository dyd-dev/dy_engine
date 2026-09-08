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
		BindingIndex bindlessTransformStorageBinding = 11;
		BindingIndex skinInfluenceStorageBinding = 13;
		BindingIndex skinPaletteStorageBinding = 14;
		BindingIndex drawConstantsBinding = 15;
		uint32_t extendedDescriptorBindingCount = 16;

		// 디스크립터 풀 크기 산정용 개수
		uint32_t materialTextureBindingCount = 5;

		// 푸시 상수 범위/오프셋 (바이트)
		// 208 = 192(기존) + 16(bindless 텍스처 디스크립터 인덱스용 float4 textureIndices)
		uint32_t pushConstantRangeSize = 208;
		uint32_t drawMetadataPushConstantOffset = 132;
	};
}
