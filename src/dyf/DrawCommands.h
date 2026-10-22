#pragma once

#include "ShaderLayout.h"
#include "dyf/Material.h"
#include "dyf/RHI/IDevice.h"
#include "dyf/RHI/ICommandList.h"
#include <cstring>

namespace dyf
{
// CPU 데이터를 버퍼의 앞부분으로 업로드하도록 기록하고 CopyDestination에서 실제 사용 상태로 배리어를 추가한다.
inline bool RecordBufferUpload(RHI::IDevice* device, RHI::ICommandList& commands,
		RHI::BufferHandle buffer, const void* data, uint32_t sizeBytes, RHI::ResourceState useState)
	{
		if(device == nullptr || buffer == nullptr || data == nullptr || sizeBytes == 0) return false; // 장치·버퍼·원본 포인터가 없거나 0바이트이면 기록하지 않고 실패한다.
		if(!device->UpdateBuffer(commands, buffer, 0, data, sizeBytes)) return false; // 업로드 기록을 받아들이지 못하면 ready 배리어를 추가하지 않는다.
		// 복사 이후 정점/인덱스/셰이더 읽기 등 useState로 전환한다. CPU에서 GPU 완료를 기다리는 호출이 아니다.
		const RHI::ResourceBarrierDesc ready = {
			buffer, nullptr, RHI::ResourceState::CopyDestination, useState, {}
		};
		commands.ResourceBarrier(&ready, 1);
		return true;
	}

// 행렬·재질 스칼라·텍스처 플래그를 셰이더와 약속한 DrawConstants 배치로 묶는다. 이 함수는 CPU 값만 만들며 명령 기록이나 자원 소유권 변경은 하지 않는다.
inline DrawConstants MakeDrawConstants(const Math::float4x4& viewProjection,
		const MaterialDesc& material, const Math::float4x4& transform, uint32_t textureFlags)
	{
		DrawConstants constants = {};
		constants.viewProjectionMatrix = viewProjection;
		constants.modelMatrix = transform;
		constants.textureFlags = textureFlags;
		// 알파 마스크 임계값을 float 비트 그대로 기존 32비트 슬롯에 저장한다. 숫자형 캐스팅 대신 memcpy를 써 셰이더의 재해석과 맞춘다.
		static_assert(sizeof(constants.padding0) == sizeof(material.alphaCutoff));
		std::memcpy(&constants.padding0, &material.alphaCutoff, sizeof(constants.padding0));
		// 표면에 수직인 방향(법선)을 담은 텍스처는 Y축 반전 여부를 별도 비트로 전달한다.
		// alphaMode·양면(4)·법선 Y 반전(8)·unlit(16)을 한 슬롯에 묶으므로 셰이더의 비트 해석과 일치해야 한다.
		constants.padding1 = static_cast<uint32_t>(material.alphaMode) | (material.doubleSided ? 4u : 0u) |
			(material.normalYSign < 0 ? 8u : 0u) | (material.unlit ? 16u : 0u);
		constants.emissiveColor = {material.emissiveColor.x, material.emissiveColor.y, material.emissiveColor.z, 0};
		constants.baseColor = material.baseColor;
		constants.materialParams = {material.metallicFactor, material.roughnessFactor,
			material.normalScale, material.occlusionStrength};
		return constants;
	}
}
