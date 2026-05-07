#include "OBJLoader.h"
#include <fstream>
#include <sstream>
#include <map>
#include <array>

namespace dy::Graphics
{
    MeshData OBJLoader::Load(const std::string& filepath)
    {
        MeshData result;

        std::ifstream file(filepath);
        if(!file.is_open()) return result;

        std::vector<std::array<float, 3>> positions;
        std::vector<std::array<float, 2>> texCoords;
        std::vector<std::array<float, 3>> normals;

        std::map<std::string, uint32_t> vertexMap;

        std::string line;
        while(std::getline(file, line))
        {
            std::istringstream ss(line);
            std::string token;
            ss >> token;

            if(token == "v")
            {
                float x, y, z;
                ss >> x >> y >> z;
                positions.push_back({x, y, z});
            }
            else if(token == "vt")
            {
                float u, v;
                ss >> u >> v;
                texCoords.push_back({u, v});
            }
            else if(token == "vn")
            {
                float nx, ny, nz;
                ss >> nx >> ny >> nz;
                normals.push_back({nx, ny, nz});
            }
            else if(token == "f")
            {
                std::string faceTokens[3];
                ss >> faceTokens[0] >> faceTokens[1] >> faceTokens[2];

                for(int i = 0; i < 3; i++)
                {
                    if(vertexMap.count(faceTokens[i]))
                    {
                        result.indices.push_back(vertexMap[faceTokens[i]]);
                        continue;
                    }

                    int posIdx = 0, uvIdx = 0, normalIdx = 0;
                    std::string temp = faceTokens[i];
                    std::replace(temp.begin(), temp.end(), '/', ' ');
                    std::istringstream fss(temp);
                    fss >> posIdx >> uvIdx >> normalIdx;

                    posIdx--;
                    uvIdx--;
                    normalIdx--;

                    MeshVertex vert{};

                    if(posIdx >= 0 && posIdx < (int)positions.size())
                    {
                        vert.position[0] = positions[posIdx][0];
                        vert.position[1] = positions[posIdx][1];
                        vert.position[2] = positions[posIdx][2];
                    }
                    if(uvIdx >= 0 && uvIdx < (int)texCoords.size())
                    {
                        vert.texCoord[0] = texCoords[uvIdx][0];
                        vert.texCoord[1] = 1.0f - texCoords[uvIdx][1];
                    }
                    if(normalIdx >= 0 && normalIdx < (int)normals.size())
                    {
                        vert.normal[0] = normals[normalIdx][0];
                        vert.normal[1] = normals[normalIdx][1];
                        vert.normal[2] = normals[normalIdx][2];
                    }

                    uint32_t newIndex = static_cast<uint32_t>(result.vertices.size());
                    result.vertices.push_back(vert);
                    result.indices.push_back(newIndex);
                    vertexMap[faceTokens[i]] = newIndex;
                }
            }
        }

        return result;
    }
}
