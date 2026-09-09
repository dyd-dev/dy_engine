// 미구현 Compute 기능의 공개 RHI 사용안. 재설계 중인 API를 사용하며 현재 빌드 대상이 아니다.
// 추가 수명 API: DiscardCommandList(미제출 목록 반환), WaitIdle(GPU 사용 종료까지 대기).
// WaitIdle은 device loss를 포함해 더 이상 자원에 접근하지 않는 상태에서 반환하는 noexcept 계약이다.
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "RHI/Binding.h"
#include "RHI/Buffer.h"
#include "RHI/ICommandList.h"
#include "RHI/IDevice.h"
#include "RHI/Pipeline.h"
#include "RHI/ResourceSet.h"
#include "RHI/Shader.h"

// 이 예제는 아직 없는 Compute 계약을 먼저 고정한다.
// 셰이더 계약: local_size_x=64, binding 0의 uint buffer에 index * index를 기록한다.
// index >= 1024인 invocation은 반환한다. dispatch 크기와 kernel ABI는 호출자가 맞춘다.
// 이 최소 예제는 완료를 기다리는 ReadBuffer를 사용한다.
// 복사 가능 여부는 usage로 명시하고, barrier로 읽기 전 상태를 전환한다.

namespace
{
	using namespace dy;

	constexpr uint32_t ElementCount = 1024;
	constexpr uint32_t ThreadsPerGroup = 64;

	[[nodiscard]] std::vector<std::byte> ReadBinary(const std::filesystem::path& path)
	{
		std::ifstream stream(path, std::ios::binary | std::ios::ate);
		if(!stream) throw std::runtime_error("셰이더 바이너리를 열 수 없습니다: " + path.string());
		const std::streamsize size = stream.tellg();
		if(size <= 0) throw std::runtime_error("셰이더 바이너리가 비어 있습니다: " + path.string());
		stream.seekg(0, std::ios::beg);
		std::vector<std::byte> result(static_cast<std::size_t>(size));
		if(!stream.read(reinterpret_cast<char*>(result.data()), size))
			throw std::runtime_error("셰이더 바이너리를 읽을 수 없습니다: " + path.string());
		return result;
	}

	void Require(bool condition, const char* message)
	{
		if(!condition) throw std::runtime_error(message);
	}

	// 제안 수명 계약: Submit은 닫힌 목록을 소비한다. 미제출 목록은 명시적으로 반환한다.
	class CommandList
	{
	public:
		explicit CommandList(RHI::IDevice& device) : m_device(device), m_list(device.AcquireCommandList())
		{
			Require(m_list != nullptr, "Failed to acquire command list");
		}
		~CommandList() { if(m_list != nullptr) m_device.DiscardCommandList(m_list); }
		CommandList(const CommandList&) = delete;
		CommandList& operator=(const CommandList&) = delete;
		RHI::ICommandList* operator->() const { return m_list; }
		RHI::ICommandList& operator*() const { return *m_list; }
		void Submit()
		{
			m_list->Close();
			RHI::ICommandList* lists[] = { m_list };
			m_list = nullptr;
			Require(m_device.Submit(lists, 1), "Command submission failed");
		}
	private:
		RHI::IDevice& m_device;
		RHI::ICommandList* m_list;
	};

	struct Resources
	{
		explicit Resources(RHI::IDevice& owner) : device(owner) {}
		RHI::IDevice& device;
		RHI::ResourceSetHandle resources = nullptr;
		RHI::BufferHandle resultBuffer = nullptr;
		RHI::PipelineHandle pipeline = nullptr;
		RHI::ShaderHandle shader = nullptr;
		~Resources()
		{
			device.WaitIdle();
			if(resources != nullptr) device.DestroyResourceSet(resources);
			if(resultBuffer != nullptr) device.DestroyBuffer(resultBuffer);
			if(pipeline != nullptr) device.DestroyPipeline(pipeline);
			if(shader != nullptr) device.DestroyShader(shader);
		}
	};
}

int main(int argc, char** argv)
{
	try
	{
		if(argc != 3) throw std::invalid_argument("Compute shader-binary entry-point");
		// Compute-only 사용은 surface나 native window를 요구하지 않는다.
		std::unique_ptr<dy::RHI::IDevice> device(dy::RHI::IDevice::Create());
		Require(device != nullptr, "RHI device 생성에 실패했습니다.");

		if(!device->Supports(dy::RHI::Feature::Compute))
		{
			std::cout << "Compute is unsupported on this device\n";
			return 2;
		}

		Resources owned(*device);
		const std::vector<std::byte> binary = ReadBinary(argv[1]);
		dy::RHI::ShaderHandle& shader = owned.shader;
		shader = device->CreateShader({
			dy::RHI::ShaderStage::Compute,
			argv[2],
			binary.data(),
			binary.size()
		});
		Require(shader != nullptr, "compute shader 생성에 실패했습니다.");

		const dy::RHI::ResourceBindingLayout resultLayout = {
			0,
			dy::RHI::ResourceBindingType::ReadWriteStorageBuffer,
			1,
			dy::RHI::ShaderStageFlags::Compute,
			{}
		};
		dy::RHI::ComputePipelineDesc pipelineDesc = {};
		pipelineDesc.computeShader = shader;
		pipelineDesc.layout = { &resultLayout, 1, 0, dy::RHI::ShaderStageFlags::None, 0 };
		dy::RHI::PipelineHandle& pipeline = owned.pipeline;
		pipeline = device->CreateComputePipeline(pipelineDesc);
		Require(pipeline != nullptr, "compute pipeline 생성에 실패했습니다.");

		dy::RHI::BufferHandle& resultBuffer = owned.resultBuffer;
		resultBuffer = device->CreateBuffer({
			ElementCount * static_cast<uint32_t>(sizeof(uint32_t)),
			static_cast<uint32_t>(sizeof(uint32_t)),
			dy::RHI::BufferUsage::Storage | dy::RHI::BufferUsage::CopySource,
			dy::RHI::ResourceState::UnorderedAccess
		});
		Require(resultBuffer != nullptr, "결과 buffer 생성에 실패했습니다.");

		const dy::RHI::ResourceBinding resultBinding = {
			0, 0, resultBuffer, nullptr, 0,
			ElementCount * static_cast<uint32_t>(sizeof(uint32_t)), {}
		};
		dy::RHI::ResourceSetHandle& resources = owned.resources;
		resources = device->CreateResourceSet({
			pipeline, &resultBinding, 1
		});
		Require(resources != nullptr, "compute resource set 생성에 실패했습니다.");

		CommandList commandList(*device);
		commandList->BindComputePipeline(pipeline);
		commandList->BindResourceSet(resources);
		commandList->Dispatch((ElementCount + ThreadsPerGroup - 1) / ThreadsPerGroup, 1, 1);
		const dy::RHI::ResourceBarrierDesc beforeReadback = {
			resultBuffer, nullptr,
			dy::RHI::ResourceState::UnorderedAccess,
			dy::RHI::ResourceState::CopySource,
			{}
		};
		commandList->ResourceBarrier(&beforeReadback, 1);
		commandList.Submit();

		// ReadBuffer는 이 최소 예제에서는 완료를 기다리는 blocking readback이다.
		std::array<uint32_t, ElementCount> result = {};
		Require(device->ReadBuffer(
			resultBuffer, 0, result.data(), static_cast<uint32_t>(sizeof(result))),
			"결과 readback에 실패했습니다.");
		for(uint32_t index = 0; index < ElementCount; ++index)
			Require(result[index] == index * index, "compute 결과가 예상과 다릅니다.");

		std::cout << "compute readback 검증 성공\n";
		return 0;
	}
	catch(const std::exception& exception)
	{
		std::cerr << exception.what() << '\n';
		return 1;
	}
}

// 필요한 공개 RHI 추가:
// - window 없이 IDevice::Create() 가능한 headless device 생성
// - Feature::Compute, IDevice::Supports
// - ShaderStage::Compute, ShaderStageFlags::Compute
// - ComputePipelineDesc, IDevice::CreateComputePipeline
// - ICommandList::BindComputePipeline, Dispatch
// - ResourceState::CopySource, IDevice::ReadBuffer(blocking)
// - BufferUsage::CopySource: readback에 사용할 buffer의 복사 용도를 명시
