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
        float normal[3]; // 정점 법선 벡터 추가
    };

    class OBJLoader
    {
    public:
        // OBJ 파싱 (기존)
        static bool Load(const std::string& filepath, std::vector<Vertex>& outVertices, std::vector<uint32_t>& outIndices, std::string* outTexturePath = nullptr);

    };
}