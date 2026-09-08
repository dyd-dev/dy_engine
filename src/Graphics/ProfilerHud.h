#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "Math/Math.h"

namespace dy::RHI
{
	class IBuffer;
	class ICommandList;
	class IDevice;
	class IPipelineState;
}

namespace dy::Graphics
{
	struct ProfilerHudMetrics
	{
		double fps = 0.0;
		double frameMilliseconds = 0.0;
		double cpuRenderMilliseconds = 0.0;
		double gpuMainMilliseconds = 0.0;
		bool hasGpuMain = false;
		uint64_t liveBuffers = 0;
		uint64_t createdBuffers = 0;
		uint64_t destroyedBuffers = 0;
		uint64_t liveTextures = 0;
		uint64_t createdTextures = 0;
		uint64_t destroyedTextures = 0;
		uint64_t livePipelines = 0;
		uint64_t createdPipelines = 0;
		uint64_t destroyedPipelines = 0;
	};

	// Small dependency-free profiler overlay. Text uses an embedded 5x7 font and
	// graphs are regular RHI triangle geometry, so the exact same code is used by
	// Metal, Vulkan and D3D12.
	class ProfilerHud final
	{
	public:
		void Initialize(RHI::IDevice* device, bool startsExpanded);
		void Shutdown(RHI::IDevice* device);
		void ToggleExpanded() { m_expanded = !m_expanded; }
		[[nodiscard]] bool IsExpanded() const { return m_expanded; }

		void PrepareFrame(
			RHI::IDevice* device,
			const ProfilerHudMetrics& metrics,
			uint32_t width,
			uint32_t height,
			bool clipYFlip);
		void Record(
			RHI::ICommandList* commandList,
			RHI::IPipelineState* pipeline,
			RHI::IBuffer* lightingBuffer,
			RHI::IBuffer* shadowMatrixBuffer) const;

	private:
		static constexpr uint32_t kHistorySize = 120u;

		struct Batch
		{
			uint32_t firstIndex = 0;
			uint32_t indexCount = 0;
			Math::float4 color = {};
		};

		struct FrameResources
		{
			RHI::IBuffer* vertexBuffer = nullptr;
			RHI::IBuffer* indexBuffer = nullptr;
			RHI::IBuffer* lightingBuffer = nullptr;
			uint32_t vertexCapacityBytes = 0;
			uint32_t indexCapacityBytes = 0;
		};

		void PushHistory(const ProfilerHudMetrics& metrics);
		void BuildGeometry(uint32_t width, uint32_t height, bool clipYFlip, const ProfilerHudMetrics& metrics);
		void EnsureAndUpload(RHI::IDevice* device, FrameResources& resources);

		std::vector<FrameResources> m_frames;
		std::vector<float> m_frameHistory;
		std::vector<float> m_cpuHistory;
		std::vector<float> m_gpuHistory;
		uint32_t m_historyCursor = 0;
		uint32_t m_historyCount = 0;
		uint32_t m_currentFrame = 0;
		uint32_t m_viewportWidth = 0;
		uint32_t m_viewportHeight = 0;
		bool m_expanded = false;
		bool m_hasGpuMain = false;

	public: // Shared with the implementation's small geometry builder.
		struct HudVertex
		{
			float px = 0.0f, py = 0.0f, pz = 0.0f;
			float nx = 0.0f, ny = 0.0f, nz = 1.0f;
			float u = 0.0f, v = 0.0f;
			float tx = 1.0f, ty = 0.0f, tz = 0.0f, tw = 1.0f;
		};

	private:
		std::vector<HudVertex> m_vertices;
		std::vector<uint32_t> m_indices;
		std::vector<Batch> m_batches;
	};
}
