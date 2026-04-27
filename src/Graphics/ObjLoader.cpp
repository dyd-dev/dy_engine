#include "ObjLoader.h"
#include <fstream>
#include <sstream>
#include <filesystem>
#include <map>
#include <array>

namespace dy::Graphics
{
    bool ObjLoader::Load(const std::string& filepath, std::vector<Vertex>& outVertices, std::vector<uint32_t>& outIndices, std::string* outTexturePath)
    {
        std::ifstream file(filepath);
        if (!file.is_open()) return false;

        std::vector<float> positions;
        std::vector<float> texCoords;
        std::string line;
        uint32_t currentIndex = 0;

        std::map<std::string, std::array<float, 3>> materials;
        std::array<float, 3> currentColor = {1.0f, 1.0f, 1.0f};

        while (std::getline(file, line))
        {
            std::istringstream ss(line);
            std::string type;
            ss >> type;

            if (type == "mtllib")
            {
                std::string mtllib;
                ss >> mtllib;
                
                std::filesystem::path objPath(filepath);
                std::filesystem::path mtlPath = objPath.parent_path() / mtllib;
                
                std::ifstream mtlFile(mtlPath);
                if (mtlFile.is_open()) {
                    std::string mtlLine;
                    std::string currentMtlName = "";
                    while (std::getline(mtlFile, mtlLine)) {
                        std::istringstream mtlSs(mtlLine);
                        std::string mtlType;
                        mtlSs >> mtlType;
                        if (mtlType == "newmtl") {
                            mtlSs >> currentMtlName;
                            materials[currentMtlName] = {1.0f, 1.0f, 1.0f};
                        }
                        else if (mtlType == "Kd" && !currentMtlName.empty()) {
                            float r, g, b;
                            mtlSs >> r >> g >> b;
                            materials[currentMtlName] = {r, g, b};
                        }
                        else if (mtlType == "map_Kd" && outTexturePath) {
                            std::string textureFile;
                            mtlSs >> textureFile;
                            *outTexturePath = (objPath.parent_path() / textureFile).string();
                        }
                    }
                }
            }
            else if (type == "usemtl")
            {
                std::string mtlName;
                ss >> mtlName;
                if (materials.find(mtlName) != materials.end()) {
                    currentColor = materials[mtlName];
                }
            }
            else if (type == "v")
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

                    vertex.color[0] = currentColor[0];
                    vertex.color[1] = currentColor[1];
                    vertex.color[2] = currentColor[2];

                    outVertices.push_back(vertex);
                    outIndices.push_back(currentIndex++);
                }
            }
        }
        return true;
    }
}