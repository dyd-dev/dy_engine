#pragma once
#include <vector>
#include <string>
#include <cstdint>
#include "Graphics/OBJLoader.h" // For dy::Graphics::Vertex

namespace dy::Graphics
{
    class GLTFLoader
    {
    public:
        // GLTF 파싱
        // outTexturePath: 파싱된 glTF 파일 등에서 찾은 베이스 컬러 텍스처의 경로를 반환합니다.
        static bool Load(const std::string& filepath, std::vector<Vertex>& outVertices, std::vector<uint32_t>& outIndices, std::string* outTexturePath = nullptr);
    };
}
