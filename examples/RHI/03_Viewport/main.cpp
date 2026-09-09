#include "Platform/Window.h"
#include "Graphics/ShaderLayout.h"
#include "vertex.h"
#include "fragment.h"
#include "Graphics/Texture.h"
#include "RHI/Buffer.h"
#include "RHI/ICommandList.h"
#include "RHI/IDevice.h"
#include "RHI/Pipeline.h"
#include "RHI/ResourceScope.h"
#include "RHI/ResourceSet.h"
#include "RHI/Shader.h"
#include "RHI/Texture.h"
#include <array>
#include <cmath>
#include <cstddef>
#include <memory>
#include <stdexcept>
#include <vector>
#include <iostream>

int main()
{
    using namespace dy::RHI;
    try
    {
        constexpr uint32_t windowWidth = 640, windowHeight = 480;
        dy::Platform::Window window(windowWidth, windowHeight, "RHI / Viewport");
        std::unique_ptr<IDevice> deviceOwner(IDevice::Create(window.GetHandle()));
        if (!deviceOwner) throw std::runtime_error("RHI device creation failed.");
        auto& device = *deviceOwner;
        ResourceScope resources(device);
        SwapchainDesc swapchain;
        swapchain.format = Format::B8G8R8A8_UNORM;
        swapchain.minimumImageCount = 2;
        swapchain.presentMode = PresentMode::Fifo;
        if (!device.CreateSwapchain(swapchain)) throw std::runtime_error("Swapchain creation failed.");
        using Vertex = dy::Graphics::ShaderLayout::CanvasVertex;
        using Color = std::array<float, 4>;
        struct Draw { uint32_t first, count; Viewport viewport; Rect scissor; };
        std::vector<Vertex> vertexData;
        std::vector<Draw> draws;
        Viewport viewport{0, 0, static_cast<float>(windowWidth), static_cast<float>(windowHeight), 0, 1};
        Rect scissor{0, 0, windowWidth, windowHeight};
        auto triangle = [&](float ax, float ay, float bx, float by, float cx, float cy, Color color) {
            draws.push_back({static_cast<uint32_t>(vertexData.size()), 3, viewport, scissor});
            for (const auto& p : std::array<std::array<float, 2>, 3>{{{ax, ay}, {bx, by}, {cx, cy}}})
                vertexData.push_back({p[0], p[1], 0, 0, color[0], color[1], color[2], color[3]});
        };
        auto quad = [&](float x, float y, float width, float height, Color color,
            float u = 0, float v = 0, float uw = 1, float vh = 1) {
            draws.push_back({static_cast<uint32_t>(vertexData.size()), 6, viewport, scissor});
            for (const auto& p : std::array<std::array<float, 2>, 6>{{{0, 0}, {1, 0}, {0, 1}, {0, 1}, {1, 0}, {1, 1}}})
                vertexData.push_back({x + p[0] * width, y + p[1] * height, u + p[0] * uw, v + p[1] * vh,
                    color[0], color[1], color[2], color[3]});
        };
        auto* vertexShader = resources.Keep(device.CreateShader({ShaderStage::Vertex,
            ShaderData::vertexEntryPoint, ShaderData::vertex, ShaderData::vertexSize}));
        auto* fragmentShader = resources.Keep(device.CreateShader({ShaderStage::Fragment,
            ShaderData::fragmentEntryPoint, ShaderData::fragment, ShaderData::fragmentSize}));
        const VertexBufferLayout input{0,sizeof(Vertex),VertexStepMode::Vertex};
        const std::array<VertexAttribute,3> attributes={{{0,0,Format::R32G32_FLOAT,0},{1,0,Format::R32G32_FLOAT,8},{2,0,Format::R32G32B32A32_FLOAT,16}}};
        SamplerDesc sampler;sampler.minFilter=sampler.magFilter=sampler.mipFilter=SamplerFilter::Linear;
        sampler.addressU=sampler.addressV=sampler.addressW=SamplerAddressMode::ClampToEdge;
        sampler.minLod=sampler.maxLod=sampler.mipLodBias=0;
        const std::array<ResourceBindingLayout,2> bindingLayout={{{0,ResourceBindingType::SampledTexture,1,ShaderStageFlags::Fragment,{}},
            {1,ResourceBindingType::StaticSampler,1,ShaderStageFlags::Fragment,sampler}}};
        const ColorAttachmentDesc output{swapchain.format,{true,BlendFactor::SourceAlpha,BlendFactor::OneMinusSourceAlpha,BlendOp::Add,
            BlendFactor::One,BlendFactor::OneMinusSourceAlpha,BlendOp::Add},ColorWriteMask::All};
        GraphicsPipelineDesc desc;desc.vertexShader=vertexShader;desc.fragmentShader=fragmentShader;desc.topology=PrimitiveTopology::TriangleList;
        desc.vertexBuffers=&input;desc.vertexBufferCount=1;desc.vertexAttributes=attributes.data();desc.vertexAttributeCount=attributes.size();
        desc.raster={FillMode::Solid,CullMode::None,FrontFace::CounterClockwise,0,0,0};desc.colorAttachments=&output;desc.colorAttachmentCount=1;
        desc.layout={bindingLayout.data(),static_cast<uint32_t>(bindingLayout.size()),16,ShaderStageFlags::Vertex,15};
        auto* pipeline = resources.Keep(device.CreateGraphicsPipeline(desc));
        if(!pipeline) throw std::runtime_error("RHI pipeline creation failed.");
        dy::Graphics::TextureAsset image{"", 1, 1, {255, 255, 255, 255}};
        viewport={0,0,320,480,0,1};scissor={0,0,320,480};
        quad(20,20,280,440,{0.1f,0.2f,0.4f,1});
        scissor={60,100,200,240};
        for(uint32_t i=0;i<64;++i) {
            const float a=i*6.283185307f/64,b=(i+1)*6.283185307f/64;
            triangle(160,240,160+150*std::cos(a),240+150*std::sin(a),160+150*std::cos(b),240+150*std::sin(b),{1,0.4f,0.2f,1});
        }
        viewport={320,0,320,480,0,1};scissor={320,0,320,480};
        quad(20,20,280,440,{0.15f,0.35f,0.2f,1});
        const float length=std::hypot(320.0f,480.0f),nx=-480/length*6,ny=320/length*6;
        triangle(nx,ny,320+nx,480+ny,-nx,-ny,{1,1,1,1});
        triangle(-nx,-ny,320+nx,480+ny,320-nx,480-ny,{1,1,1,1});
        // Explicit resource creation, upload, and state transitions.
        auto* vertices=resources.Keep(device.CreateBuffer({static_cast<uint32_t>(vertexData.size()*sizeof(Vertex)),sizeof(Vertex),BufferUsage::Vertex,ResourceState::CopyDestination}));
        TextureDesc textureDesc;textureDesc.width=image.width;textureDesc.height=image.height;textureDesc.depthOrArraySize=textureDesc.mipLevels=1;
        textureDesc.format=Format::R8G8B8A8_UNORM;textureDesc.usage=TextureUsage::ShaderResource;
        auto* texture=resources.Keep(device.CreateTexture(textureDesc));
        if(!vertices || !texture)throw std::runtime_error("Resource creation failed.");
        auto* upload=device.AcquireCommandList();
        if(!upload)throw std::runtime_error("Upload command list unavailable.");
        ResourceBarrierDesc textureBarrier{nullptr,texture,ResourceState::Undefined,ResourceState::CopyDestination,{}};
        upload->ResourceBarrier(&textureBarrier,1);
        const bool uploaded=device.UpdateBuffer(*upload,vertices,0,vertexData.data(),vertices->GetDesc().size)
            && device.UpdateTexture(*upload,texture,0,0,image.rgba8.data(),static_cast<uint32_t>(image.rgba8.size()),image.width*4,image.width*image.height*4);
        const std::array<ResourceBarrierDesc,2> ready={{{vertices,nullptr,ResourceState::CopyDestination,ResourceState::VertexBuffer,{}},
            {nullptr,texture,ResourceState::CopyDestination,ResourceState::ShaderResource,{}}}};
        upload->ResourceBarrier(ready.data(),ready.size());upload->Close();
        if(!device.Submit(&upload,1) || !uploaded)throw std::runtime_error("Upload failed.");
        ResourceBinding binding;binding.binding=0;binding.texture=texture;
        auto* bindings=resources.Keep(device.CreateResourceSet({pipeline,&binding,1}));
        if(!bindings)throw std::runtime_error("Resource bindings failed.");

        while (window.IsRunning()) {
            window.PollEvents();
            if (!window.IsRunning()) break;
            if (!device.BeginFrame()) continue;
            auto* commands=device.AcquireCommandList();
            if(!commands)throw std::runtime_error("Frame command list unavailable.");
            auto* target=device.GetBackBuffer();
            const ResourceBarrierDesc begin{nullptr,target,ResourceState::Present,ResourceState::RenderTarget,{}};
            commands->ResourceBarrier(&begin,1);
            ColorAttachment color; color.texture=target;color.loadOp=LoadOp::Clear;color.storeOp=StoreOp::Store;
            color.clearColor[0]=0.04f;color.clearColor[1]=0.06f;color.clearColor[2]=0.10f;color.clearColor[3]=1;
            commands->BeginRendering({&color,1,nullptr});
            commands->BindGraphicsPipeline(pipeline);
            commands->BindResourceSet(bindings);
            commands->BindVertexBuffer(0,vertices,0);
            for(const Draw& draw:draws)
            {
                commands->SetViewport(draw.viewport);
                commands->SetScissor(draw.scissor);
                const float transform[4]={2/draw.viewport.width,-2/draw.viewport.height,-1,1};
                commands->SetInlineConstants(0,sizeof(transform),transform);
                commands->DrawInstanced(draw.count,1,draw.first,0);
            }
            commands->EndRendering();
            const ResourceBarrierDesc end{nullptr,target,ResourceState::RenderTarget,ResourceState::Present,{}};
            commands->ResourceBarrier(&end,1);commands->Close();
            if(!device.Submit(&commands,1))throw std::runtime_error("Draw submission failed.");
            device.Present();
        }
    }
    catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
