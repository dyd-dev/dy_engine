#include "Platform/Window.h"
#include "RHI/IDevice.h"
#include "RHI/ICommandList.h"
#include "RHI/IBuffer.h"
#include "RHI/IPipelineState.h"
#include "Graphics/ObjLoader.h"
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
    uint32_t pad1, pad2, pad3;
};

int main()
{
    Platform::Window window(800, 600, "D3D12_Test");

    // RHI 디바이스 초기화
    RHI::IDevice* device = RHI::IDevice::Create(window.GetHandle());
    if (!device) return -1;

    // OBJ 파싱
    std::vector<Graphics::Vertex> vertices;
    std::vector<uint32_t> indices;

    std::string texturePath;
    if (!Graphics::ObjLoader::Load("examples/D3D12_Test/Lowpoly_tree.obj", vertices, indices, &texturePath)) {
        std::cerr << "OBJ 로드 실패!" << std::endl;
		system("pause");
        return -1;
    }

    // 정점 버퍼 생성 및 데이터 복사
    RHI::BufferDesc vbDesc = {};
    vbDesc.size = static_cast<uint32_t>(vertices.size() * sizeof(Graphics::Vertex));
    vbDesc.stride = sizeof(Graphics::Vertex);
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


    // 셰이더 소스 읽기
    std::string vsSource = ReadTextFile("examples/D3D12_Test/Shaders/TestVS.hlsl");
    std::string psSource = ReadTextFile("examples/D3D12_Test/Shaders/TestPS.hlsl");
    
    if (vsSource.empty() || psSource.empty()) {
        std::cerr << "셰이더 파일 로드 실패!" << std::endl;
        system("pause");
        return -1;
    }
    
    // NULL 문자 추가 (D3DCompile이 문자열 끝을 알 수 있게)
    vsSource.push_back('\0');
    psSource.push_back('\0');

    // PSO 디스크립터 채우기
    RHI::GraphicsPipelineDesc psoDesc = {};
    psoDesc.vertexShader = vsSource.data();
    psoDesc.vertexShaderSize = vsSource.size() - 1; // 널문자 제외
    psoDesc.pixelShader = psSource.data();
    psoDesc.pixelShaderSize = psSource.size() - 1;


    // 실제 PSO 생성
    RHI::IPipelineState* pso = device->CreateGraphicsPipeline(psoDesc);
    if(!pso) {
		std::cerr << "PSO 생성 실패!" << std::endl;
		system("pause");
		return -1;
    }


    
    // 텍스처 로드 (STB Image)
    int texWidth = 1, texHeight = 1, texChannels = 4;
    stbi_set_flip_vertically_on_load(true);
    if (texturePath.empty()) {
        texturePath = "examples/D3D12_Test/Lowpoly_tree.png"; // Fallback
    }
    unsigned char* pixels = stbi_load(texturePath.c_str(), &texWidth, &texHeight, &texChannels, 4);
    
    RHI::ITexture* modelTexture = nullptr;
    uint32_t texSlot = dy::RHI::INVALID_DESCRIPTOR_INDEX;
    
    RHI::TextureDesc texDesc = {};
    texDesc.width = texWidth;
    texDesc.height = texHeight;
    texDesc.format = RHI::Format::R8G8B8A8_UNORM;
    texDesc.usage = RHI::TextureUsage::ShaderResource;
    modelTexture = device->CreateTexture(texDesc);
    
    if (pixels) {
        device->UpdateTexture(modelTexture, pixels, texWidth * 4);
        stbi_image_free(pixels);
    } else {
        std::cout << "텍스처 이미지가 지정되지 않았습니다. 기본 흰색(단색) 텍스처를 적용합니다." << std::endl;
        unsigned char defaultPixels[4] = { 255, 255, 255, 255 }; // 흰색 (1x1)
        device->UpdateTexture(modelTexture, defaultPixels, 4);
    }
    
    texSlot = device->AllocateDescriptorSlot();
    device->UpdateDescriptorSlot(texSlot, modelTexture);

    while (window.IsRunning())
    {
        window.PollEvents();

        TransformData transform = { 0.0f, -0.8f, 0.5f, 1.0f, texSlot, 0, 0, 0 };

        device->BeginFrame();

        RHI::ICommandList* cmdList = device->AcquireCommandList();

        RHI::ITexture* backBuffer = device->GetBackBuffer();
        cmdList->SetRenderTargets(1, &backBuffer, nullptr);
        cmdList->ClearColor(backBuffer, 0.0f, 0.2f, 0.4f, 1.0f);

        if (pso) {
            cmdList->BindGraphicsPipeline(pso);
            cmdList->BindGlobalDescriptorHeap();
            cmdList->SetPushConstants(sizeof(TransformData), &transform);

            if (vertexBuffer) {
                cmdList->BindVertexBuffer(vertexBuffer);
            }

            if (indexBuffer) {
                cmdList->BindIndexBuffer(indexBuffer, dy::RHI::Format::R32_UINT, 0);
            }

            if(!indices.empty()) {
                cmdList->DrawIndexedInstanced(static_cast<uint32_t>(indices.size()), 1, 0, 0, 0);
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