#pragma once
#include <vector>
#include <string>
#include <cstdint>

namespace dy::Graphics
{
    // D3D12, Vulkan 모두 공통으로 쓸 엔진 표준 정점 구조체
    struct Vertex {
        float position[3];
        float texCoord[2];
        float color[3];
    };

    class ObjLoader
    {
    public:
        // 정적(static) 함수로 만들어서 객체 생성 없이 바로 쓸 수 있게 합니다.
        // outTexturePath: 파싱된 mtl 파일 등에서 찾은 텍스처(diffuse map)의 경로를 반환합니다.
        static bool Load(const std::string& filepath, std::vector<Vertex>& outVertices, std::vector<uint32_t>& outIndices, std::string* outTexturePath = nullptr);
    };
}