#include "Platform/Window.h"
#include "RHI/IDevice.h"
#include "RHI/ICommandList.h"
#include "RHI/IBuffer.h"
#include "RHI/IPipelineState.h"
#include "Graphics/GLTFLoader.h"
#include "RHI/ITexture.h"
#include <iostream>
#include <vector>
#include <cstring>
#include <fstream>
#include <string>

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>
#include "Math/Math.h"
#include <cmath>

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

namespace
{
    float Dot(const Math::float3& a, const Math::float3& b)
    {
        return a.x * b.x + a.y * b.y + a.z * b.z;
    }

    Math::float3 Cross(const Math::float3& a, const Math::float3& b)
    {
        return Math::float3(
            a.y * b.z - a.z * b.y,
            a.z * b.x - a.x * b.z,
            a.x * b.y - a.y * b.x
        );
    }

    Math::float3 Normalize(const Math::float3& value)
    {
        const float length = std::sqrt(Dot(value, value));
        if (length <= 0.00001f) return Math::float3(0.0f, 0.0f, 0.0f);
        return Math::float3(value.x / length, value.y / length, value.z / length);
    }

    Math::float4x4 MultiplyColumnMajor(const Math::float4x4& lhs, const Math::float4x4& rhs)
    {
        Math::float4x4 result = {};
        for (int column = 0; column < 4; ++column)
        {
            for (int row = 0; row < 4; ++row)
            {
                float value = 0.0f;
                for (int k = 0; k < 4; ++k)
                {
                    value += lhs.m[k * 4 + row] * rhs.m[column * 4 + k];
                }
                result.m[column * 4 + row] = value;
            }
        }
        return result;
    }

    Math::float4x4 CreateLookAtLH(const Math::float3& eye, const Math::float3& target, const Math::float3& up)
    {
        const Math::float3 forward = Normalize(Math::float3(target.x - eye.x, target.y - eye.y, target.z - eye.z));
        const Math::float3 right = Normalize(Cross(up, forward));
        const Math::float3 cameraUp = Cross(forward, right);

        Math::float4x4 view = Math::float4x4::Identity();
        view.m[0] = right.x;
        view.m[4] = right.y;
        view.m[8] = right.z;
        view.m[12] = -Dot(right, eye);

        view.m[1] = cameraUp.x;
        view.m[5] = cameraUp.y;
        view.m[9] = cameraUp.z;
        view.m[13] = -Dot(cameraUp, eye);

        view.m[2] = forward.x;
        view.m[6] = forward.y;
        view.m[10] = forward.z;
        view.m[14] = -Dot(forward, eye);
        return view;
    }

    Math::float4x4 CreateOrthographicLH(float width, float height, float nearPlane, float farPlane)
    {
        Math::float4x4 projection = Math::float4x4::Identity();
        projection.m[0] = 2.0f / width;
        projection.m[5] = 2.0f / height;
        projection.m[10] = 1.0f / (farPlane - nearPlane);
        projection.m[14] = -nearPlane / (farPlane - nearPlane);
        return projection;
    }
}

struct alignas(16) TransformData {
    Math::float4x4 viewProj;
    float offsetX, offsetY, offsetZ, offsetW;
    Math::float4 lightDir;
    Math::float4 spotLightPos;
    uint32_t textureIndex;
    uint32_t vertexBufferIndex;
    uint32_t indexBufferIndex;
    uint32_t drawMode; // 0: Model, 1: Floor, 2: Shadow
    float rotationY;
    float aspect;      // 화면 종횡비 보정값 (Height / Width)
    float pad[2];
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
    Platform::Window window(1280, 720, "D3D12_Lighting");

    // RHI 디바이스 초기화
    RHI::IDevice* device = RHI::IDevice::Create(window.GetHandle());
    if (!device) return -1;

    // GLTF 파싱
    Graphics::MeshData mesh;

    std::string texturePath;
    if (!Graphics::GLTFLoader::Load("examples/D3D12_Lighting/Models/shiba/scene.gltf", mesh, &texturePath)) {
        std::cerr << "Failed to load GLTF file!" << std::endl;
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
    std::string vsSource = ReadTextFile("examples/D3D12_Lighting/Shaders/TestVS.hlsl");
    std::string psSource = ReadTextFile("examples/D3D12_Lighting/Shaders/TestPS.hlsl");
    
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
    
    if (!texturePath.empty()) {
        unsigned char* pixels = stbi_load(texturePath.c_str(), &texWidth, &texHeight, &texChannels, 4);
        if (pixels) {
            RHI::TextureDesc texDesc = {};
            texDesc.width = texWidth;
            texDesc.height = texHeight;
            texDesc.format = RHI::Format::R8G8B8A8_UNORM;
            texDesc.usage = RHI::TextureUsage::ShaderResource;
            modelTexture = device->CreateTexture(texDesc);
            device->UpdateTexture(modelTexture, pixels, texWidth * 4);
            stbi_image_free(pixels);
        }
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

        // 매 프레임 회전 각도 증가 (방향광 회전용)
        rotationAngle += 0.015f;

        // 방향광 회전 방향 계산
        float lightAngle = rotationAngle * 1.5f;
        Math::float4 lightDir(std::cos(lightAngle), 1.2f, std::sin(lightAngle), 0.0f);
        // 스포트라이트 위치 (시바견 머리 위쪽 Y=1.8f)
        Math::float4 spotLightPos(0.0f, 1.8f, 0.5f, 0.0f);

        // 시바견 모델 위치 조정 및 회전값 설정
        // glTF 모델의 실제 발끝이 원점보다 훨씬 아래(약 Y=-1.0f 부근)에 있습니다.
        // 따라서 Y=1.1f로 충분히 올려주어야 발바닥이 바닥 평면(Y=0.0f) 위에 완벽히 서게 됩니다.
        // 모델 자체는 회전하지 않고 고정시킵니다.
        TransformData transform = {};
        transform.offsetX = 0.0f;
        transform.offsetY = 1.1f;
        transform.offsetZ = 0.5f;
        transform.offsetW = 1.0f;
        transform.textureIndex = texSlot;
        transform.vertexBufferIndex = vbSlot;
        transform.indexBufferIndex = ibSlot;
        transform.lightDir = lightDir;
        transform.spotLightPos = spotLightPos;
        transform.rotationY = 0.0f; // 모델 고정

        device->BeginFrame();

        RHI::ICommandList* cmdList = device->AcquireCommandList();

        RHI::ITexture* backBuffer = device->GetBackBuffer();
        cmdList->SetRenderTargets(1, &backBuffer, nullptr);
        cmdList->ClearColor(backBuffer, 0.025f, 0.035f, 0.052f, 1.0f); // 어두운 남회색 배경
        cmdList->ClearDepth(nullptr, 1.0f);

        if (pso) {
            cmdList->BindGraphicsPipeline(pso);
            cmdList->BindGlobalDescriptorHeap();

            // 세 개의 뷰포트 정보 정의
            struct ViewportConfig {
                RHI::Viewport vp;
                RHI::Rect scissor;
                Math::float4x4 viewProj;
            } configs[3];

            Math::float3 target = { 0.0f, 1.1f, 0.5f };

            // 1. Isometric View (얼굴이 보이도록 Z 위치를 target.z - 2.0f 앞쪽으로 조정)
            Math::float3 isoPos = { target.x + 2.0f, target.y + 1.5f, target.z - 2.0f };
            Math::float4x4 isoView = CreateLookAtLH(isoPos, target, {0.0f, 1.0f, 0.0f});
            Math::float4x4 isoProj = CreateOrthographicLH(3.5f, 2.8f, 0.1f, 10.0f);
            configs[0] = { {0.0f, 0.0f, 896.0f, 720.0f, 0.0f, 1.0f}, {0, 0, 896, 720}, MultiplyColumnMajor(isoProj, isoView) };

            // 2. Side View
            Math::float3 sidePos = { target.x + 2.5f, target.y, target.z };
            Math::float4x4 sideView = CreateLookAtLH(sidePos, target, {0.0f, 1.0f, 0.0f});
            Math::float4x4 sideProj = CreateOrthographicLH(3.5f, 3.3f, 0.1f, 10.0f);
            configs[1] = { {896.0f, 0.0f, 384.0f, 360.0f, 0.0f, 1.0f}, {896, 0, 384, 360}, MultiplyColumnMajor(sideProj, sideView) };

            // 3. Top View
            Math::float3 topPos = { target.x, target.y + 2.5f, target.z + 0.001f };
            Math::float4x4 topView = CreateLookAtLH(topPos, target, {0.0f, 0.0f, -1.0f});
            Math::float4x4 topProj = CreateOrthographicLH(3.5f, 3.3f, 0.1f, 10.0f);
            configs[2] = { {896.0f, 360.0f, 384.0f, 360.0f, 0.0f, 1.0f}, {896, 360, 384, 360}, MultiplyColumnMajor(topProj, topView) };

            // 각 뷰포트별로 렌더링 루프 수행
            for (int i = 0; i < 3; ++i) {
                cmdList->SetViewport(configs[i].vp);
                cmdList->SetScissor(configs[i].scissor);
                transform.viewProj = configs[i].viewProj;

                // 뷰포트 종횡비 보정값 (높이 / 너비) 설정
                transform.aspect = configs[i].vp.height / configs[i].vp.width;

                // 시바견 본체 위치 정보 초기화
                transform.offsetX = 0.0f;
                transform.offsetY = 1.1f;
                transform.offsetZ = 0.5f;
                transform.offsetW = 1.0f;

                // Pass 1: 바닥(Floor) 그리기
                transform.drawMode = 1; // Floor
                cmdList->SetPushConstants(sizeof(TransformData), &transform);
                cmdList->DrawInstanced(6, 1, 0, 0); // 6개의 버텍스로 평평한 쿼드 바닥 생성

                // Pass 2: 그림자(Shadow) 그리기
                if (!indices.empty()) {
                    transform.drawMode = 2; // Shadow
                    cmdList->SetPushConstants(sizeof(TransformData), &transform);
                    cmdList->DrawInstanced(static_cast<uint32_t>(indices.size()), 1, 0, 0);
                }

                // Pass 3: 모델(Shiba Inu) 그리기
                if (!indices.empty()) {
                    transform.drawMode = 0; // Model
                    cmdList->SetPushConstants(sizeof(TransformData), &transform);
                    cmdList->DrawInstanced(static_cast<uint32_t>(indices.size()), 1, 0, 0);
                }

                // Pass 4: 스포트라이트 조명 마커 그리기 (절차적 황금빛 구체)
                {
                    transform.offsetX = spotLightPos.x;
                    transform.offsetY = spotLightPos.y;
                    transform.offsetZ = spotLightPos.z;
                    transform.offsetW = 0.02f;  // 구체 크기 조절 인자
                    transform.drawMode = 3;     // Spotlight marker (Procedural Sphere)
                    cmdList->SetPushConstants(sizeof(TransformData), &transform);
                    cmdList->DrawInstanced(6, 1, 0, 0); // 6개 정점으로 카메라 대면 빌보드 쿼드 생성
                }

                // Pass 5: 방향광 조명 마커 그리기 (절차적 해님 구체 - 궤도 회전)
                {
                    Math::float3 dirLightPos(
                        target.x + lightDir.x * 1.3f,
                        target.y + lightDir.y * 1.3f,
                        target.z + lightDir.z * 1.3f
                    );
                    transform.offsetX = dirLightPos.x;
                    transform.offsetY = dirLightPos.y;
                    transform.offsetZ = dirLightPos.z;
                    transform.offsetW = 0.022f; // 구체 크기 조절 인자
                    transform.drawMode = 4;     // Directional Light marker (Procedural Sphere)
                    cmdList->SetPushConstants(sizeof(TransformData), &transform);
                    cmdList->DrawInstanced(6, 1, 0, 0); // 6개 정점으로 카메라 대면 빌보드 쿼드 생성
                }
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
