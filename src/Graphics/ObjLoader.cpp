#include "ObjLoader.h"
#include <fstream>
#include <sstream>

namespace dy::Graphics
{
    bool ObjLoader::Load(const std::string& filepath, std::vector<Vertex>& outVertices, std::vector<uint32_t>& outIndices)
    {
        std::ifstream file(filepath);
        if (!file.is_open()) return false;

        std::vector<float> positions;
        std::vector<float> texCoords;
        std::string line;
        uint32_t currentIndex = 0;

        while (std::getline(file, line))
        {
            std::istringstream ss(line);
            std::string type;
            ss >> type;

            if (type == "v")
            {
                float x, y, z;
                ss >> x >> y >> z;
                positions.push_back(x); positions.push_back(y); positions.push_back(z);
            }
            else if (type == "vt")
            {
                float u, v;
                ss >> u >> v;
                texCoords.push_back(u); texCoords.push_back(v);
            }
            else if (type == "f")
            {
                std::string vertexStr;
                for (int i = 0; i < 3; ++i)
                {
                    ss >> vertexStr;
                    std::istringstream vs(vertexStr);
                    std::string posIdxStr, texIdxStr;

                    std::getline(vs, posIdxStr, '/');
                    std::getline(vs, texIdxStr, '/');

                    int posIdx = std::stoi(posIdxStr) - 1;
                    int texIdx = (!texIdxStr.empty()) ? std::stoi(texIdxStr) - 1 : 0;

                    Vertex vertex;
                    vertex.position[0] = positions[posIdx * 3 + 0];
                    vertex.position[1] = positions[posIdx * 3 + 1];
                    vertex.position[2] = positions[posIdx * 3 + 2];

                    if (!texCoords.empty()) {
                        vertex.texCoord[0] = texCoords[texIdx * 2 + 0];
                        vertex.texCoord[1] = 1.0f - texCoords[texIdx * 2 + 1];
                    }
                    else {
                        vertex.texCoord[0] = 0.0f; vertex.texCoord[1] = 0.0f;
                    }

                    outVertices.push_back(vertex);
                    outIndices.push_back(currentIndex++);
                }
            }
        }
        return true;
    }
}