#pragma once

namespace dy::RHI
{
	class IDevice;

	class IBuffer;
	class IPipelineState;
}

namespace dy
{
	class JobSystem;
}

namespace dy::Graphics
{
	class Renderer
	{
	private:
		// GPU Buffers mapped to CPU memory
		RHI::IBuffer* m_globalTransformBuffer = NULL;
		RHI::IBuffer* m_globalMaterialBuffer = NULL;

		RHI::IPipelineState* m_opaquePSO = NULL;

		static constexpr uint32_t MAX_SUPPORTED_ENTITIES = 100000;

	public:
		Renderer() = default;
		~Renderer() = default;

		void Initialize(RHI::IDevice* device);
		void Shutdown(RHI::IDevice* device);

		void SubmitFrame(const Scene& scene, RHI::IDevice* device, JobSystem* jobSystem);

	private:
		void BuildPipelineStates(RHI::IDevice* device);
	};
}