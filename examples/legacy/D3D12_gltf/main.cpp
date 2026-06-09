#include "Platform/Window.h"
#include "RHI/IDevice.h"
#include "RHI/ICommandList.h"
#include "RHI/IBuffer.h"
#include "RHI/IPipelineState.h"
#include "RHI/ITexture.h"
#include "Graphics/ImageFile.h"
#include "Graphics/Mesh.h"

#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

using namespace dy;

std::string ReadTextFile(const char* filepath)
{
    std::ifstream file(filepath, std::ios::binary);
    if (!file.is_open()) return "";
    file.seekg(0, std::ios::end);
    size_t size = file.tellg();
    file.seekg(0, std::ios::beg);
    std::string content(size, '\0');
    file.read(content.data(), size);
    return content;
}

struct alignas(16) TransformData
{
    float offsetX, offsetY, offsetZ, offsetW;
    uint32_t textureIndex;
    uint32_t vertexBufferIndex;
    uint32_t indexBufferIndex;
    float rotationY;
};

struct D3DModelVertex
{
    float position[3];
    float uv[2];
    float color[3];
    float normal[3];
};

std::vector<D3DModelVertex> BuildD3DVertices(const Graphics::MeshData& mesh)
{
    std::vector<D3DModelVertex> vertices;
    vertices.reserve(mesh.vertices.size());

    for (const Graphics::Vertex& vertex : mesh.vertices)
    {
        vertices.push_back({
            { vertex.position.x, vertex.position.y, vertex.position.z },
            { vertex.uv.x, vertex.uv.y },
            { vertex.color.x, vertex.color.y, vertex.color.z },
            { vertex.normal.x, vertex.normal.y, vertex.normal.z },
        });
    }

    return vertices;
}

RHI::ITexture* CreateTextureFromFileOrWhite(RHI::IDevice* device, const std::string& path)
{
    Graphics::ImageFile image = Graphics::LoadImageFile(path);
    RHI::TextureDesc texDesc = {};
    texDesc.width = image.IsValid() ? image.GetWidth() : 1u;
    texDesc.height = image.IsValid() ? image.GetHeight() : 1u;
    texDesc.format = RHI::Format::R8G8B8A8_UNORM;
    texDesc.usage = RHI::TextureUsage::ShaderResource;

    RHI::ITexture* texture = device->CreateTexture(texDesc);
    if (image.IsValid())
    {
        device->UpdateTexture(texture, image.GetPixels().data(), image.GetRowPitch());
    }
    else
    {
        const unsigned char white[4] = { 255, 255, 255, 255 };
        device->UpdateTexture(texture, white, 4);
    }
    return texture;
}

int main()
{
    Platform::Window window(800, 600, "D3D12_GLTF_Test");

    RHI::IDevice* device = RHI::IDevice::Create(window.GetHandle());
    if (!device) return -1;

    Graphics::MeshData mesh;
    std::string texturePath;
    if (!Graphics::LoadMesh("examples/D3D12_gltf/Models/shiba/scene.gltf", mesh, &texturePath))
    {
        std::cerr << "Failed to load GLTF file!" << std::endl;
        system("pause");
        return -1;
    }

    std::vector<D3DModelVertex> vertices = BuildD3DVertices(mesh);
    const std::vector<uint32_t>& indices = mesh.indices;

    RHI::BufferDesc vbDesc = {};
    vbDesc.size = static_cast<uint32_t>(vertices.size() * sizeof(D3DModelVertex));
    vbDesc.stride = sizeof(D3DModelVertex);
    vbDesc.usage = RHI::BufferUsage::Vertex;
    RHI::IBuffer* vertexBuffer = device->CreateBuffer(vbDesc);

    void* mappedVB = vertexBuffer->Map(0);
    std::memcpy(mappedVB, vertices.data(), vbDesc.size);
    vertexBuffer->Unmap();

    RHI::BufferDesc ibDesc = {};
    ibDesc.size = static_cast<uint32_t>(indices.size() * sizeof(uint32_t));
    ibDesc.stride = sizeof(uint32_t);
    ibDesc.usage = RHI::BufferUsage::Index;
    RHI::IBuffer* indexBuffer = device->CreateBuffer(ibDesc);

    void* mappedIB = indexBuffer->Map(0);
    std::memcpy(mappedIB, indices.data(), ibDesc.size);
    indexBuffer->Unmap();

    const uint32_t vbSlot = device->AllocateDescriptorSlot();
    device->UpdateDescriptorSlot(vbSlot, vertexBuffer);

    const uint32_t ibSlot = device->AllocateDescriptorSlot();
    device->UpdateDescriptorSlot(ibSlot, indexBuffer);

    std::string vsSource = ReadTextFile("examples/D3D12_gltf/Shaders/TestVS.hlsl");
    std::string psSource = ReadTextFile("examples/D3D12_gltf/Shaders/TestPS.hlsl");
    if (vsSource.empty() || psSource.empty())
    {
        std::cerr << "Failed to load shader files!" << std::endl;
        system("pause");
        return -1;
    }

    RHI::GraphicsPipelineDesc psoDesc = {};
    psoDesc.vertexShader = vsSource.data();
    psoDesc.vertexShaderSize = vsSource.size();
    psoDesc.pixelShader = psSource.data();
    psoDesc.pixelShaderSize = psSource.size();

    RHI::IPipelineState* pso = device->CreateGraphicsPipeline(psoDesc);
    if (!pso)
    {
        std::cerr << "Failed to create PSO!" << std::endl;
        system("pause");
        return -1;
    }

    RHI::ITexture* modelTexture = CreateTextureFromFileOrWhite(device, texturePath);
    const uint32_t texSlot = device->AllocateDescriptorSlot();
    device->UpdateDescriptorSlot(texSlot, modelTexture);

    float rotationAngle = 0.0f;
    while (window.IsRunning())
    {
        window.PollEvents();
        rotationAngle += 0.01f;

        TransformData transform = { 0.0f, 0.3f, 0.5f, 1.0f, texSlot, vbSlot, ibSlot, rotationAngle };

        device->BeginFrame();

        RHI::ICommandList* cmdList = device->AcquireCommandList();
        RHI::ITexture* backBuffer = device->GetBackBuffer();
        cmdList->SetRenderTargets(1, &backBuffer, nullptr);
        cmdList->ClearColor(backBuffer, 0.1f, 0.1f, 0.1f, 1.0f);
        cmdList->ClearDepth(nullptr, 1.0f);

        if (pso && !indices.empty())
        {
            cmdList->BindGraphicsPipeline(pso);
            cmdList->BindGlobalDescriptors();
            cmdList->SetInlineConstants(sizeof(TransformData), &transform);
            cmdList->DrawInstanced(static_cast<uint32_t>(indices.size()), 1, 0, 0);
        }

        cmdList->Close();

        device->Submit(&cmdList, 1);
        device->Present();
    }

    delete indexBuffer;
    delete vertexBuffer;
    delete modelTexture;
    delete pso;
    delete device;

    return 0;
}
