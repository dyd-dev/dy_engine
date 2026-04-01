#pragma once

namespace dy::RHI
{
	class IDevice;
	class IPipelineState;
}

namespace dy::Graphics
{
	class Scene;
	class Camera;

	class Renderer
	{
	public:
		Renderer(RHI::IDevice *device);
		~Renderer();

		void RenderFrame();

	private:
		RHI::IDevice* m_device;
		RHI::IPipelineState* m_pipeline;
	};
}