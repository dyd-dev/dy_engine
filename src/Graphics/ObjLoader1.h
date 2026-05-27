#pragma once
#include <vector>
#include <string>

namespace dy::Graphics
{
    struct MeshVertex
    {
        float position[3];
        float texCoord[2];
        float normal[3];
    };

    struct MeshData
    {
        std::vector<MeshVertex> vertices;
        std::vector<uint32_t>   indices;
    };

    class OBJLoader
    {
    public:
        // OBJ 파일 로드 → MeshData 반환
        // 실패 시 빈 MeshData 반환
        static MeshData Load(const std::string& filepath);
    };
}
