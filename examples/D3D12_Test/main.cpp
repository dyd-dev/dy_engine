#include "Platform/Window.h"
#include "RHI/IDevice.h"
#include "RHI/ICommandList.h"
#include "RHI/IBuffer.h"
#include "RHI/IPipelineState.h"
#include "Graphics/ObjLoader.h"
#include <iostream>
#include <vector>
#include <cstring>
#include <fstream>
#include <string>

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

    if (!Graphics::ObjLoader::Load("Lowpoly_Tree.obj", vertices, indices)) {
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


    
    while (window.IsRunning())
    {
        window.PollEvents();

        TransformData transform = { 0.0f, -0.8f, 0.5f, 1.0f };

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
    delete pso;
    delete device;

    return 0;
}