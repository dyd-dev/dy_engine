#include "Graphics/Model.h" // CPU asset decoding / animation mathematics only; no Scene or Renderer.
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
#include "Graphics/Mesh.h"
#include "Graphics/Animation.h"
#include <iostream>
#include <chrono>

int main()
{
    using namespace dy::RHI;
    namespace Math = dy::Math;
    using Vertex = dy::Graphics::Vertex;
    using dy::Graphics::CreateCubeMesh;
    using DrawConstants = dy::Graphics::ShaderLayout::DrawConstants;
    using Lighting = dy::Graphics::ShaderLayout::RendererLightingConstants;
    namespace Assets = dy::Graphics;
    try {
        constexpr uint32_t windowWidth = 640, windowHeight = 480;
        dy::Platform::Window window(windowWidth, windowHeight, "RHI / Model and Animation");
        std::unique_ptr<IDevice> deviceOwner(IDevice::Create(window.GetHandle()));
        if (!deviceOwner) throw std::runtime_error("RHI device creation failed.");
        auto& device = *deviceOwner;
        ResourceScope resources(device);
        SwapchainDesc swapchain;
        swapchain.format = Format::B8G8R8A8_UNORM;
        swapchain.minimumImageCount = 2;
        swapchain.presentMode = PresentMode::Fifo;
        if (!device.CreateSwapchain(swapchain)) throw std::runtime_error("Swapchain creation failed.");
        auto* vertexShader = resources.Keep(device.CreateShader({ShaderStage::Vertex,
            ShaderData::vertexEntryPoint, ShaderData::vertex, ShaderData::vertexSize}));
        auto* fragmentShader = resources.Keep(device.CreateShader({ShaderStage::Fragment,
            ShaderData::fragmentEntryPoint, ShaderData::fragment, ShaderData::fragmentSize}));
        const VertexBufferLayout input{0, sizeof(Vertex), VertexStepMode::Vertex};
        const std::array<VertexAttribute, 4> attributes = {{
            {0, 0, Format::R32G32B32_FLOAT, offsetof(Vertex, position)},
            {1, 0, Format::R32G32B32_FLOAT, offsetof(Vertex, normal)},
            {2, 0, Format::R32G32_FLOAT, offsetof(Vertex, uv)},
            {3, 0, Format::R32G32B32A32_FLOAT, offsetof(Vertex, tangent)}
        }};
        SamplerDesc sampler;
        sampler.minFilter = sampler.magFilter = sampler.mipFilter = SamplerFilter::Linear;
        sampler.addressU = sampler.addressV = sampler.addressW = SamplerAddressMode::Repeat;
        sampler.minLod = sampler.maxLod = sampler.mipLodBias = 0;
        const std::array<ResourceBindingLayout, 9> bindingLayout = {{
            {0, ResourceBindingType::SampledTexture, 1, ShaderStageFlags::Fragment, {}},
            {1, ResourceBindingType::ConstantBuffer, 1, ShaderStageFlags::Fragment, {}},
            {4, ResourceBindingType::SampledTexture, 1, ShaderStageFlags::Fragment, {}},
            {5, ResourceBindingType::SampledTexture, 1, ShaderStageFlags::Fragment, {}},
            {6, ResourceBindingType::SampledTexture, 1, ShaderStageFlags::Fragment, {}},
            {7, ResourceBindingType::SampledTexture, 1, ShaderStageFlags::Fragment, {}},
            {8, ResourceBindingType::StaticSampler, 1, ShaderStageFlags::Fragment, sampler},
            {11, ResourceBindingType::ReadOnlyStorageBuffer, 1, ShaderStageFlags::Vertex, {}},
            {12, ResourceBindingType::ReadOnlyStorageBuffer, 1, ShaderStageFlags::Vertex, {}}
        }};
        const ColorAttachmentDesc output{swapchain.format,
            {true, BlendFactor::SourceAlpha, BlendFactor::OneMinusSourceAlpha, BlendOp::Add,
                BlendFactor::One, BlendFactor::Zero, BlendOp::Add}, ColorWriteMask::All};
        GraphicsPipelineDesc desc;
        desc.vertexShader = vertexShader;
        desc.fragmentShader = fragmentShader;
        desc.topology = PrimitiveTopology::TriangleList;
        desc.vertexBuffers = &input;
        desc.vertexBufferCount = 1;
        desc.vertexAttributes = attributes.data();
        desc.vertexAttributeCount = attributes.size();
        desc.raster = {FillMode::Solid, CullMode::Back, FrontFace::CounterClockwise, 0, 0, 0};
        desc.depthStencil.format = Format::D32_FLOAT;
        desc.depthStencil.depthTestEnabled = desc.depthStencil.depthWriteEnabled = true;
        desc.depthStencil.depthCompareOp = CompareOp::Less;
        desc.colorAttachments = &output;
        desc.colorAttachmentCount = 1;
        desc.layout = {bindingLayout.data(), static_cast<uint32_t>(bindingLayout.size()), sizeof(DrawConstants),
            ShaderStageFlags::Vertex | ShaderStageFlags::Fragment, 10};
        auto* pipeline = resources.Keep(device.CreateGraphicsPipeline(desc));
        if (!pipeline) throw std::runtime_error("Mesh pipeline creation failed.");
        TextureDesc depthDesc;
        depthDesc.width = device.GetBackBuffer()->GetDesc().width;
        depthDesc.height = device.GetBackBuffer()->GetDesc().height;
        depthDesc.format = Format::D32_FLOAT;
        depthDesc.usage = TextureUsage::DepthStencil;
        auto* depthTexture = resources.Keep(device.CreateTexture(depthDesc));
        if (!depthTexture) throw std::runtime_error("Mesh depth target creation failed.");
        ResourceState depthState = ResourceState::Undefined;
        Assets::ModelData model;
        if (!Assets::LoadModel(DY_EXAMPLE_MODEL, model) || model.animations.empty())
            throw std::runtime_error("Animated model loading failed.");

        struct Batch {
            BufferHandle vertices = nullptr, indices = nullptr;
            ResourceSetHandle bindings = nullptr;
            uint32_t indexCount = 0, meshIndex = 0;
            DrawConstants draw{};
        };
        std::vector<Batch> batches;
        std::vector<Assets::SkinInfluence> influences;
        std::vector<Assets::SkinJointMatrices> palette;
        for (uint32_t index = 0; index < model.meshes.size(); ++index) {
            const auto& source = model.meshes[index];
            if (source.mesh.vertices.empty() || source.mesh.indices.empty()) continue;
            Batch batch;
            batch.draw.modelMatrix = Math::float4x4::Identity();
            batch.draw.padding0 = batch.draw.padding1 = UINT32_MAX;
            batch.draw.baseColor = {1, 1, 1, 1};
            batch.draw.materialParams = {0, 0.5f, 1, 1};
            batch.meshIndex = index;
            batch.indexCount = source.mesh.indices.size();
            if (!source.mesh.skinInfluences.empty()) {
                if (source.skinIndex >= model.skins.size()) throw std::runtime_error("Invalid model skin.");
                batch.draw.padding0 = influences.size();
                batch.draw.padding1 = palette.size();
                influences.insert(influences.end(), source.mesh.skinInfluences.begin(), source.mesh.skinInfluences.end());
                palette.resize(palette.size() + model.skins[source.skinIndex].jointNodeIndices.size());
            }
            batches.push_back(batch);
        }
        if (influences.empty() || palette.empty()) throw std::runtime_error("Example model has no skinning data.");

        auto* upload = device.AcquireCommandList();
        if (!upload) throw std::runtime_error("Upload command list unavailable.");
        const auto influenceBufferBytes = influences.size() * sizeof(Assets::SkinInfluence);
        if (influenceBufferBytes > UINT32_MAX) throw std::runtime_error("Buffer data exceeds the RHI size range.");
        auto* influenceBuffer = resources.Keep(device.CreateBuffer({static_cast<uint32_t>(influenceBufferBytes),
            sizeof(Assets::SkinInfluence), BufferUsage::Storage, ResourceState::CopyDestination}));
        if (!device.UpdateBuffer(*upload, influenceBuffer, 0, influences.data(), influenceBuffer->GetDesc().size))
            throw std::runtime_error("Buffer upload failed.");
        const ResourceBarrierDesc influenceBufferReady{influenceBuffer, nullptr, ResourceState::CopyDestination, ResourceState::ShaderResource, {}};
        upload->ResourceBarrier(&influenceBufferReady, 1);
        const auto paletteBufferBytes = palette.size() * sizeof(Assets::SkinJointMatrices);
        if (paletteBufferBytes > UINT32_MAX) throw std::runtime_error("Buffer data exceeds the RHI size range.");
        auto* paletteBuffer = resources.Keep(device.CreateBuffer({static_cast<uint32_t>(paletteBufferBytes),
            sizeof(Assets::SkinJointMatrices), BufferUsage::Storage, ResourceState::CopyDestination}));
        if (!device.UpdateBuffer(*upload, paletteBuffer, 0, palette.data(), paletteBuffer->GetDesc().size))
            throw std::runtime_error("Buffer upload failed.");
        const ResourceBarrierDesc paletteBufferReady{paletteBuffer, nullptr, ResourceState::CopyDestination, ResourceState::ShaderResource, {}};
        upload->ResourceBarrier(&paletteBufferReady, 1);
        Lighting lighting{};
        lighting.cameraPosition = {0,-3,1.6f,0};
        lighting.directionalLightDirection = {0,-1,2,0};
        lighting.directionalLightColor = {1,1,1,3};
        lighting.ambientColor = {1,1,1,0.035f};
        lighting.shadowParams = {0.0007f,0.003f,0,1};
        lighting.pbrParams = {0.04f,0.25f,1,0};
        lighting.environmentColor = {1,1,1,1};
        lighting.lightCounts = {1,0,0,0};
        const auto direction = Math::NormalizeOr({0,-1,2},{0,0,-1});
        lighting.directionalLights[0] = {{direction.x,direction.y,direction.z,3},{1,1,1,0}};
        lighting.shadowLight = {0,0,0,0};

        lighting.cameraPosition = {2.6f, -3, 1.8f, 0};
        const auto lightBufferBytes = sizeof(lighting);
        if (lightBufferBytes > UINT32_MAX) throw std::runtime_error("Buffer data exceeds the RHI size range.");
        auto* lightBuffer = resources.Keep(device.CreateBuffer({static_cast<uint32_t>(lightBufferBytes),
            sizeof(lighting), BufferUsage::Constant, ResourceState::CopyDestination}));
        if (!device.UpdateBuffer(*upload, lightBuffer, 0, &lighting, lightBuffer->GetDesc().size))
            throw std::runtime_error("Buffer upload failed.");
        const ResourceBarrierDesc lightBufferReady{lightBuffer, nullptr, ResourceState::CopyDestination, ResourceState::ConstantBuffer, {}};
        upload->ResourceBarrier(&lightBufferReady, 1);
        const uint8_t whitePixel[] = {255,255,255,255}, normalPixel[] = {128,128,255,255}, blackPixel[] = {0,0,0,255};
        TextureDesc whiteDesc;
        whiteDesc.width = 1; whiteDesc.height = 1;
        whiteDesc.format = Format::R8G8B8A8_UNORM;
        whiteDesc.usage = TextureUsage::ShaderResource;
        const uint64_t whiteBytes = static_cast<uint64_t>(whiteDesc.width) * whiteDesc.height * 4;
        if (!whiteDesc.width || !whiteDesc.height || whiteBytes > UINT32_MAX)
            throw std::runtime_error("Texture data exceeds the RHI upload range.");
        auto* white = resources.Keep(device.CreateTexture(whiteDesc));
        ResourceBarrierDesc whiteReady{nullptr, white, ResourceState::Undefined, ResourceState::CopyDestination, {}};
        upload->ResourceBarrier(&whiteReady, 1);
        if (!device.UpdateTexture(*upload, white, 0, 0, whitePixel, static_cast<uint32_t>(whiteBytes),
            whiteDesc.width * 4, static_cast<uint32_t>(whiteBytes))) throw std::runtime_error("Texture upload failed.");
        whiteReady.before = ResourceState::CopyDestination; whiteReady.after = ResourceState::ShaderResource;
        upload->ResourceBarrier(&whiteReady, 1);
        TextureDesc normalDesc;
        normalDesc.width = 1; normalDesc.height = 1;
        normalDesc.format = Format::R8G8B8A8_UNORM;
        normalDesc.usage = TextureUsage::ShaderResource;
        const uint64_t normalBytes = static_cast<uint64_t>(normalDesc.width) * normalDesc.height * 4;
        if (!normalDesc.width || !normalDesc.height || normalBytes > UINT32_MAX)
            throw std::runtime_error("Texture data exceeds the RHI upload range.");
        auto* normal = resources.Keep(device.CreateTexture(normalDesc));
        ResourceBarrierDesc normalReady{nullptr, normal, ResourceState::Undefined, ResourceState::CopyDestination, {}};
        upload->ResourceBarrier(&normalReady, 1);
        if (!device.UpdateTexture(*upload, normal, 0, 0, normalPixel, static_cast<uint32_t>(normalBytes),
            normalDesc.width * 4, static_cast<uint32_t>(normalBytes))) throw std::runtime_error("Texture upload failed.");
        normalReady.before = ResourceState::CopyDestination; normalReady.after = ResourceState::ShaderResource;
        upload->ResourceBarrier(&normalReady, 1);
        TextureDesc blackDesc;
        blackDesc.width = 1; blackDesc.height = 1;
        blackDesc.format = Format::R8G8B8A8_UNORM;
        blackDesc.usage = TextureUsage::ShaderResource;
        const uint64_t blackBytes = static_cast<uint64_t>(blackDesc.width) * blackDesc.height * 4;
        if (!blackDesc.width || !blackDesc.height || blackBytes > UINT32_MAX)
            throw std::runtime_error("Texture data exceeds the RHI upload range.");
        auto* black = resources.Keep(device.CreateTexture(blackDesc));
        ResourceBarrierDesc blackReady{nullptr, black, ResourceState::Undefined, ResourceState::CopyDestination, {}};
        upload->ResourceBarrier(&blackReady, 1);
        if (!device.UpdateTexture(*upload, black, 0, 0, blackPixel, static_cast<uint32_t>(blackBytes),
            blackDesc.width * 4, static_cast<uint32_t>(blackBytes))) throw std::runtime_error("Texture upload failed.");
        blackReady.before = ResourceState::CopyDestination; blackReady.after = ResourceState::ShaderResource;
        upload->ResourceBarrier(&blackReady, 1);
        std::vector<TextureHandle> textures;
        for (const auto& source : model.textures) {
            if (source.rgba8.empty()) throw std::runtime_error("Model texture was not decoded.");
            TextureDesc textureDesc;
            textureDesc.width = source.width; textureDesc.height = source.height;
            textureDesc.format = Format::R8G8B8A8_UNORM;
            textureDesc.usage = TextureUsage::ShaderResource;
            const uint64_t textureBytes = static_cast<uint64_t>(textureDesc.width) * textureDesc.height * 4;
            if (!textureDesc.width || !textureDesc.height || textureBytes > UINT32_MAX)
                throw std::runtime_error("Texture data exceeds the RHI upload range.");
            auto* texture = resources.Keep(device.CreateTexture(textureDesc));
            ResourceBarrierDesc textureReady{nullptr, texture, ResourceState::Undefined, ResourceState::CopyDestination, {}};
            upload->ResourceBarrier(&textureReady, 1);
            if (!device.UpdateTexture(*upload, texture, 0, 0, source.rgba8.data(), static_cast<uint32_t>(textureBytes),
                textureDesc.width * 4, static_cast<uint32_t>(textureBytes))) throw std::runtime_error("Texture upload failed.");
            textureReady.before = ResourceState::CopyDestination; textureReady.after = ResourceState::ShaderResource;
            upload->ResourceBarrier(&textureReady, 1);
            textures.push_back(texture);
        }
        for (auto& batch : batches) {
            const auto& source = model.meshes[batch.meshIndex];
            const auto& vertices = source.mesh.vertices;
            const auto verticesBytes = vertices.size() * sizeof(Vertex);
            if (verticesBytes > UINT32_MAX) throw std::runtime_error("Buffer data exceeds the RHI size range.");
            batch.vertices = resources.Keep(device.CreateBuffer({static_cast<uint32_t>(verticesBytes),
                sizeof(Vertex), BufferUsage::Vertex, ResourceState::CopyDestination}));
            if (!device.UpdateBuffer(*upload, batch.vertices, 0, vertices.data(), batch.vertices->GetDesc().size))
                throw std::runtime_error("Buffer upload failed.");
            const ResourceBarrierDesc verticesReady{batch.vertices, nullptr, ResourceState::CopyDestination, ResourceState::VertexBuffer, {}};
            upload->ResourceBarrier(&verticesReady, 1);
            const auto indicesBytes = source.mesh.indices.size() * sizeof(uint32_t);
            if (indicesBytes > UINT32_MAX) throw std::runtime_error("Buffer data exceeds the RHI size range.");
            batch.indices = resources.Keep(device.CreateBuffer({static_cast<uint32_t>(indicesBytes),
                sizeof(uint32_t), BufferUsage::Index, ResourceState::CopyDestination}));
            if (!device.UpdateBuffer(*upload, batch.indices, 0, source.mesh.indices.data(), batch.indices->GetDesc().size))
                throw std::runtime_error("Buffer upload failed.");
            const ResourceBarrierDesc indicesReady{batch.indices, nullptr, ResourceState::CopyDestination, ResourceState::IndexBuffer, {}};
            upload->ResourceBarrier(&indicesReady, 1);
            if (source.materialIndex >= model.materials.size()) throw std::runtime_error("Invalid material index.");
            const auto& material = model.materials[source.materialIndex];
            const auto& desc = material.material;
            batch.draw.baseColor = desc.baseColor;
            batch.draw.emissiveColor = {desc.emissiveColor.x, desc.emissiveColor.y, desc.emissiveColor.z, 0};
            batch.draw.materialParams = {desc.metallicFactor, desc.roughnessFactor, desc.normalScale, desc.occlusionStrength};
            std::array<TextureHandle,5> materialTextures{white,white,normal,white,black};
            for (uint32_t kind = 0; kind < materialTextures.size(); ++kind) {
                if (material.textureIndices[kind] < textures.size()) {
                    materialTextures[kind] = textures[material.textureIndices[kind]];
                    batch.draw.textureFlags |= 1u << kind;
                }
            }
            const std::array<ResourceBinding, 8> materialBindings = {{
                {0, 0, nullptr, materialTextures[0], 0, 0, {}},
                {1, 0, lightBuffer, nullptr, 0, sizeof(Lighting), {}},
                {4, 0, nullptr, materialTextures[1], 0, 0, {}},
                {5, 0, nullptr, materialTextures[2], 0, 0, {}},
                {6, 0, nullptr, materialTextures[3], 0, 0, {}},
                {7, 0, nullptr, materialTextures[4], 0, 0, {}},
                {11, 0, influenceBuffer, nullptr, 0, influenceBuffer->GetDesc().size, {}},
                {12, 0, paletteBuffer, nullptr, 0, paletteBuffer->GetDesc().size, {}}
            }};
            batch.bindings = resources.Keep(device.CreateResourceSet({pipeline, materialBindings.data(), materialBindings.size()}));
        }
        upload->Close();
        if (!device.Submit(&upload, 1)) throw std::runtime_error("Model resource upload failed.");

        const auto view = Math::LookAtRH({2.6f,-3,1.8f}, {0,0,0.4f}, {0,0,1});
        const auto projection = Math::PerspectiveRH_ZO(1.0f, 640.0f / 480, 0.1f, 100);
        const auto root = Math::Scaling(0.012f) * Math::RotationX(1.5707963f) * model.assetTransform;
        std::vector<Assets::NodeTransform> pose;
        std::vector<Math::float4x4> globalPose;
        float animationTime = 0;
        auto lastFrame = std::chrono::steady_clock::now();
        while (window.IsRunning()) {
            window.PollEvents();
            if (!window.IsRunning()) break;
            if (!device.BeginFrame()) continue;
            pose.clear();
            for (const auto& node : model.nodes) pose.push_back(node.bindTransform);
            if (!Assets::SampleAnimationClip(model.animations[0], animationTime, pose)
                || !Assets::BuildGlobalNodeMatrices(model.nodes, pose, globalPose)) throw std::runtime_error("Animation pose evaluation failed.");
            for (auto& batch : batches) {
                const auto& mesh = model.meshes[batch.meshIndex];
                if (mesh.nodeIndex >= globalPose.size()) throw std::runtime_error("Invalid mesh node.");
                batch.draw.modelMatrix = root * globalPose[mesh.nodeIndex];
                batch.draw.viewProjectionMatrix = projection * view;
                if (batch.draw.padding0 != UINT32_MAX && mesh.skinIndex < model.skins.size()) {
                    std::vector<Assets::SkinJointMatrices> joints;
                    if (!Assets::BuildSkinPalette(globalPose[mesh.nodeIndex], globalPose, model.skins[mesh.skinIndex], joints))
                        throw std::runtime_error("Skin palette evaluation failed.");
                    std::copy(joints.begin(), joints.end(), palette.begin() + batch.draw.padding1);
                }
            }
            auto* commands = device.AcquireCommandList();
            if (!commands) throw std::runtime_error("Model command list unavailable.");
            ResourceBarrierDesc paletteBarrier{paletteBuffer,nullptr,ResourceState::ShaderResource,ResourceState::CopyDestination,{}};
            commands->ResourceBarrier(&paletteBarrier,1);
            if (!device.UpdateBuffer(*commands,paletteBuffer,0,palette.data(),paletteBuffer->GetDesc().size))
                throw std::runtime_error("Joint palette upload failed.");
            std::swap(paletteBarrier.before,paletteBarrier.after);
            commands->ResourceBarrier(&paletteBarrier,1);
            auto* target = device.GetBackBuffer();
            const std::array<ResourceBarrierDesc,2> before = {{
                {nullptr,target,ResourceState::Present,ResourceState::RenderTarget,{}},
                {nullptr,depthTexture,depthState,ResourceState::DepthWrite,{}}
            }};
            commands->ResourceBarrier(before.data(),depthState==ResourceState::DepthWrite?1:2);
            ColorAttachment color;
            color.texture=target; color.loadOp=LoadOp::Clear; color.storeOp=StoreOp::Store;
            color.clearColor[0]=0.08f; color.clearColor[1]=0.10f; color.clearColor[2]=0.14f; color.clearColor[3]=1;
            DepthStencilAttachment depth;
            depth.texture=depthTexture; depth.state=ResourceState::DepthWrite;
            depth.depthLoadOp=LoadOp::Clear; depth.depthStoreOp = StoreOp::Discard; depth.clearDepth=1;
            commands->BeginRendering({&color,1,&depth});
            commands->BindGraphicsPipeline(pipeline);
            commands->SetViewport({0,0,static_cast<float>(target->GetDesc().width),static_cast<float>(target->GetDesc().height),0,1});
            commands->SetScissor({0,0,target->GetDesc().width,target->GetDesc().height});
            for (const auto& batch : batches) {
                commands->BindResourceSet(batch.bindings);
                commands->BindVertexBuffer(0,batch.vertices,0);
                commands->BindIndexBuffer(batch.indices,Format::R32_UINT,0);
                commands->SetInlineConstants(0,sizeof(batch.draw),&batch.draw);
                commands->DrawIndexedInstanced(batch.indexCount,1,0,0,0);
            }
            commands->EndRendering();
            const ResourceBarrierDesc present{nullptr,target,ResourceState::RenderTarget,ResourceState::Present,{}};
            commands->ResourceBarrier(&present,1);
            commands->Close();
            if (!device.Submit(&commands,1)) throw std::runtime_error("Model draw submission failed.");
            depthState=ResourceState::DepthWrite;
            device.Present();
            const auto now = std::chrono::steady_clock::now();
            const float delta = std::chrono::duration<float>(now - lastFrame).count();
            lastFrame = now;
            animationTime=std::fmod(animationTime+delta,model.animations[0].duration);
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
