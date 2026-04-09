#include "Platform/Window.h"
#include "RHI/IDevice.h"
#include "RHI/ICommandList.h"
#include "RHI/IBuffer.h"
#include "RHI/IPipelineState.h"
#include "Graphics/ObjLoader.h"
#include <iostream>
#include <vector>
#include <cstring>
#include <d3dcompiler.h>
#pragma comment(lib, "d3dcompiler.lib")

using namespace dy;

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

    if (!Graphics::ObjLoader::Load("Tree.obj", vertices, indices)) {
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

    // 상수 버퍼(Constant Buffer) 생성
    // DX12의 상수 버퍼는 반드시 256바이트의 배수 크기로 생성해야 합니다.
    RHI::BufferDesc cbDesc = {};
    cbDesc.size = 256;
    RHI::IBuffer* constantBuffer = device->CreateBuffer(cbDesc);

    // 셰이더 컴파일 및 PSO 생성
    ID3DBlob* vsBlob = nullptr;
    ID3DBlob* psBlob = nullptr;
    ID3DBlob* errorBlob = nullptr;

    // VS 컴파일
    if (FAILED(D3DCompileFromFile(L"shaders.hlsl", nullptr, nullptr, "VSMain", "vs_5_0", 0, 0, &vsBlob, &errorBlob))) {
		std::cerr << "Vertex Shader 컴파일 실패!" << std::endl;
        if (errorBlob) std::cerr << (char*)errorBlob->GetBufferPointer() << std::endl;
		system("pause");    
        return -1;
    }

    // PS 컴파일
    if (FAILED(D3DCompileFromFile(L"shaders.hlsl", nullptr, nullptr, "PSMain", "ps_5_0", 0, 0, &psBlob, &errorBlob))) {
		std::cerr << "Pixel Shader 컴파일 실패!" << std::endl;
		if (errorBlob) std::cerr << (char*)errorBlob->GetBufferPointer() << std::endl;
		system("pause");
        return -1;
    }

    // PSO 디스크립터 채우기
    RHI::GraphicsPipelineDesc psoDesc = {};
    psoDesc.vertexShader = vsBlob->GetBufferPointer();
    psoDesc.vertexShaderSize = vsBlob->GetBufferSize();
    psoDesc.pixelShader = psBlob->GetBufferPointer();
    psoDesc.pixelShaderSize = psBlob->GetBufferSize();

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

        TransformData transform = { 0.0f, -0.8f, 0.0f, 1.0f };
		void* mappedCB = constantBuffer->Map(0, cbDesc.size);
		memcpy(mappedCB, &transform, sizeof(TransformData));
		constantBuffer->Unmap();

        device->BeginFrame();

        RHI::ICommandList* cmdList = device->AcquireCommandList();

        cmdList->ClearColor(nullptr, 0.0f, 0.2f, 0.4f, 1.0f);
		cmdList->SetViewport(800.0f, 600.0f);

        if (pso) {
            cmdList->BindGraphicsPipeline(pso);

			cmdList->SetConstantBuffer(0, constantBuffer); // b0 레지스터에 상수 버퍼 바인딩

            if (vertexBuffer) {
                cmdList->BindVertexBuffer(vertexBuffer);
            }

            if (indexBuffer) {
                cmdList->BindIndexBuffer(indexBuffer, dy::RHI::Format::Unknown, 0);
            }

            if(!indices.empty()) {
                cmdList->DrawIndexedInstanced(static_cast<uint32_t>(indices.size()), 1, 0, 0, 0);
			}        
        }
    
        cmdList->Close();

        device->Submit(&cmdList, 1);
        device->Present();

		delete cmdList;
    }
    
	delete indexBuffer;
    delete vertexBuffer;
	delete constantBuffer;
    delete pso;
    delete device;

    return 0;
}