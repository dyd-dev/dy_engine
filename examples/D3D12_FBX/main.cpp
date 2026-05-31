#include "Platform/Window.h"
#include "RHI/IDevice.h"
#include "RHI/ICommandList.h"
#include "RHI/IBuffer.h"
#include "RHI/IPipelineState.h"
#include "Graphics/FBXLoader.h"
#include "RHI/ITexture.h"
#include <iostream>
#include <vector>
#include <cstring>
#include <fstream>
#include <string>

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

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

struct alignas(16) TransformData {
    float offsetX, offsetY, offsetZ, offsetW;
    uint32_t textureIndex;
    uint32_t vertexBufferIndex;
    uint32_t indexBufferIndex;
    float rotationY; // 회전각
};

struct D3DModelVertex {
    float position[3];
    float uv[2];
    float color[3];
    float normal[3];
};

std::vector<D3DModelVertex> BuildD3DVertices(const Graphics::MeshData& mesh)
{
    std::vector<D3DModelVertex> vertices;
    vertices.reserve(mesh.vertices.size());

    for (const Graphics::Vertex& vertex : mesh.vertices) {
        vertices.push_back({
            { vertex.position.x, vertex.position.y, vertex.position.z },
            { vertex.uv.x, vertex.uv.y },
            { 1.0f, 1.0f, 1.0f },
            { vertex.normal.x, vertex.normal.y, vertex.normal.z },
        });
    }

    return vertices;
}

int main()
{
    Platform::Window window(800, 600, "D3D12_FBX_Test");

    // RHI 디바이스 초기화
    RHI::IDevice* device = RHI::IDevice::Create(window.GetHandle());
    if (!device) return -1;

    // FBX 파싱
    Graphics::MeshData mesh;

    std::string texturePath;
    // 모델 경로
    std::string fbxPath = "examples/D3D12_FBX/Models/shiba/scene.FBX";
    if (!Graphics::FBXLoader::Load(fbxPath, mesh, &texturePath)) {
        std::cerr << "Failed to load FBX file: " << fbxPath << std::endl;
        std::cerr << "Please ensure the FBX model exists in the Models directory." << std::endl;
        system("pause");
        return -1;
    }
    std::vector<D3DModelVertex> vertices = BuildD3DVertices(mesh);
    const std::vector<uint32_t>& indices = mesh.indices;

    // 정점 버퍼 생성 및 데이터 복사
    RHI::BufferDesc vbDesc = {};
    vbDesc.size = static_cast<uint32_t>(vertices.size() * sizeof(D3DModelVertex));
    vbDesc.stride = sizeof(D3DModelVertex);
    vbDesc.usage = RHI::BufferUsage::Vertex;
    RHI::IBuffer* vertexBuffer = device->CreateBuffer(vbDesc);

    void* mappedVB = vertexBuffer->Map(0, vbDesc.size);
    memcpy(mappedVB, vertices.data(), vbDesc.size);
    vertexBuffer->Unmap();

    // 인덱스 버퍼 생성 및 데이터 복사
    RHI::BufferDesc ibDesc = {};
    ibDesc.size = static_cast<uint32_t>(indices.size() * sizeof(uint32_t));
    ibDesc.stride = sizeof(uint32_t);
    ibDesc.usage = RHI::BufferUsage::Index;
    RHI::IBuffer* indexBuffer = device->CreateBuffer(ibDesc);

    void* mappedIB = indexBuffer->Map(0, ibDesc.size);
    memcpy(mappedIB, indices.data(), ibDesc.size);
    indexBuffer->Unmap();

    // SRV 할당
    uint32_t vbSlot = device->AllocateDescriptorSlot();
    device->UpdateDescriptorSlot(vbSlot, vertexBuffer);

    uint32_t ibSlot = device->AllocateDescriptorSlot();
    device->UpdateDescriptorSlot(ibSlot, indexBuffer);

    // 셰이더 소스 읽기
    std::string vsSource = ReadTextFile("examples/D3D12_FBX/Shaders/TestVS.hlsl");
    std::string psSource = ReadTextFile("examples/D3D12_FBX/Shaders/TestPS.hlsl");
    
    if (vsSource.empty() || psSource.empty()) {
        std::cerr << "Failed to load shader files!" << std::endl;
        system("pause");
        return -1;
    }
    
    vsSource.push_back('\0');
    psSource.push_back('\0');

    // PSO 디스크립터 채우기
    RHI::GraphicsPipelineDesc psoDesc = {};
    psoDesc.vertexShader = vsSource.data();
    psoDesc.vertexShaderSize = vsSource.size() - 1;
    psoDesc.pixelShader = psSource.data();
    psoDesc.pixelShaderSize = psSource.size() - 1;

    // 실제 PSO 생성
    RHI::IPipelineState* pso = device->CreateGraphicsPipeline(psoDesc);
    if(!pso) {
        std::cerr << "Failed to create PSO!" << std::endl;
        system("pause");
        return -1;
    }

    // 텍스처 로드 (STB Image)
    int texWidth = 1, texHeight = 1, texChannels = 4;
    stbi_set_flip_vertically_on_load(true);
    
    RHI::ITexture* modelTexture = nullptr;
    uint32_t texSlot = dy::RHI::INVALID_DESCRIPTOR_INDEX;
    
    // 텍스처 로드
    std::string targetTexturePath = "examples/D3D12_FBX/Models/shiba/textures/default_Base_Color.png";
    unsigned char* pixels = stbi_load(targetTexturePath.c_str(), &texWidth, &texHeight, &texChannels, 4);
    if (pixels) {
        std::cout << "Successfully loaded texture: " << targetTexturePath << std::endl;
        RHI::TextureDesc texDesc = {};
        texDesc.width = texWidth;
        texDesc.height = texHeight;
        texDesc.format = RHI::Format::R8G8B8A8_UNORM;
        texDesc.usage = RHI::TextureUsage::ShaderResource;
        modelTexture = device->CreateTexture(texDesc);
        device->UpdateTexture(modelTexture, pixels, texWidth * 4);
        stbi_image_free(pixels);
    }

    if (!modelTexture) {
        std::cout << "Using default white texture." << std::endl;
        RHI::TextureDesc texDesc = {};
        texDesc.width = 1;
        texDesc.height = 1;
        texDesc.format = RHI::Format::R8G8B8A8_UNORM;
        texDesc.usage = RHI::TextureUsage::ShaderResource;
        modelTexture = device->CreateTexture(texDesc);
        unsigned char defaultPixels[4] = { 255, 255, 255, 255 };
        device->UpdateTexture(modelTexture, defaultPixels, 4);
    }
    
    texSlot = device->AllocateDescriptorSlot();
    device->UpdateDescriptorSlot(texSlot, modelTexture);

    float rotationAngle = 0.0f;

    while (window.IsRunning())
    {
        window.PollEvents();

        // 매 프레임 회전 각도 증가
        rotationAngle += 0.01f;

        // FBX 모델 위치 조정 및 회전값 설정
        TransformData transform = { 0.0f, 0.0f, 0.5f, 1.0f, texSlot, vbSlot, ibSlot, rotationAngle };

        device->BeginFrame();

        RHI::ICommandList* cmdList = device->AcquireCommandList();

        RHI::ITexture* backBuffer = device->GetBackBuffer();
        cmdList->SetRenderTargets(1, &backBuffer, nullptr);
        cmdList->ClearColor(backBuffer, 0.15f, 0.15f, 0.15f, 1.0f);
        cmdList->ClearDepth(nullptr, 1.0f);

        if (pso) {
            cmdList->BindGraphicsPipeline(pso);
            cmdList->BindGlobalDescriptorHeap();
            cmdList->SetPushConstants(sizeof(TransformData), &transform);

            if(!indices.empty()) {
                cmdList->DrawInstanced(static_cast<uint32_t>(indices.size()), 1, 0, 0);
            }        
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
