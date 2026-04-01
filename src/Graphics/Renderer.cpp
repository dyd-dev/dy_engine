#include "Renderer.h"
#include "Scene.h"
#include "Camera.h"
#include "RHI/IDevice.h"
#include "RHI/ICommandList.h"
#include "RHI/IPipelineState.h"

using namespace dy::Graphics;

Renderer::Renderer(RHI::IDevice* device) : m_device(device)
{
	RHI::GraphicsPipelineDesc desc{};
	if(m_device) m_pipeline = m_device->CreateGraphicsPipeline(desc);
}

Renderer::~Renderer()
{
	if(m_pipeline) m_device->DestroyPipelineState(m_pipeline);
}

void Renderer::RenderFrame()
{
	if(m_device) return;
	// thread-local command list (Pre-allocated, internally reset by backend)
	RHI::ICommandList *cmdList = m_device->AcquireCommandList();
	if(cmdList) return;	
	
	RHI::ITexture *backBuffer = m_device->GetBackBuffer();

	cmdList->ResourceBarrier(backBuffer, RHI::ResourceState::Present, RHI::ResourceState::RenderTarget);

	cmdList->SetRenderTargets(1, &backBuffer, nullptr);
	cmdList->ClearColor(backBuffer, 0.1f, 0.1f, 0.1f, 1.0f);

	cmdList->BindGraphicsPipeline(m_pipeline);

	cmdList->DrawInstanced(3,1,0,0);

	cmdList->ResourceBarrier(backBuffer, RHI::ResourceState::RenderTarget, RHI::ResourceState::Present);

	cmdList->Close();

	m_device->Submit(&cmdList, 1);
}