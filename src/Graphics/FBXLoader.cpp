#include "FBXLoader.h"
#include <ufbx.h>
#include <iostream>
#include <filesystem>

namespace dy::Graphics
{
    bool FBXLoader::Load(const std::string& filepath, std::vector<Vertex>& outVertices, std::vector<uint32_t>& outIndices, std::string* outTexturePath)
    {
        ufbx_load_opts opts = {};
        // 옵션 설정: 외부 참조 파일 로드 허용 등
        opts.load_external_files = true;
        
        ufbx_error error;
        ufbx_scene* scene = ufbx_load_file(filepath.c_str(), &opts, &error);
        if (!scene) {
            std::cerr << "Failed to load FBX file: " << filepath << "\nReason: " << error.info << std::endl;
            return false;
        }

        // 1. 텍스처 경로 추출 (첫 번째 메터리얼 기준)
        if (outTexturePath && outTexturePath->empty()) {
            for (size_t i = 0; i < scene->materials.count; ++i) {
                ufbx_material* mat = scene->materials.data[i];
                std::string texRelPath;

                if (mat->pbr.base_color.texture) {
                    texRelPath = mat->pbr.base_color.texture->relative_filename.data;
                } else if (mat->fbx.diffuse_color.texture) {
                    texRelPath = mat->fbx.diffuse_color.texture->relative_filename.data;
                }

                if (!texRelPath.empty()) {
                    *outTexturePath = (std::filesystem::path(filepath).parent_path() / texRelPath).string();
                    break;
                }
            }
        }

        // 2. 씬의 노드 순회하며 메쉬 인스턴스 로드
        uint32_t currentIndex = 0;

        for (size_t ni = 0; ni < scene->nodes.count; ++ni) {
            ufbx_node* node = scene->nodes.data[ni];
            if (!node->mesh) continue;

            ufbx_mesh* mesh = node->mesh;

            // 각 다각형(Face)을 순회하며 삼각화(Triangulation) 수행
            for (size_t fi = 0; fi < mesh->faces.count; ++fi) {
                ufbx_face face = mesh->faces.data[fi];
                if (face.num_indices < 3) continue;

                // 최대 생성될 수 있는 삼각형 인덱스 개수 계산: (N - 2) * 3
                size_t maxTriIndices = (face.num_indices - 2) * 3;
                std::vector<uint32_t> triIndices(maxTriIndices);

                size_t numTris = ufbx_triangulate_face(triIndices.data(), triIndices.size(), mesh, face);

                // 삼각화된 인덱스를 기반으로 정점 데이터 조립
                for (size_t ti = 0; ti < numTris * 3; ++ti) {
                    uint32_t index = triIndices[ti];

                    Vertex vertex = {};

                    // 위치(Position) 가져오기 및 노드 글로벌 행렬 적용
                    ufbx_vec3 localPos = ufbx_get_vertex_vec3(&mesh->vertex_position, index);
                    ufbx_vec3 worldPos = ufbx_transform_position(&node->geometry_to_world, localPos);

                    vertex.position[0] = static_cast<float>(worldPos.x);
                    vertex.position[1] = static_cast<float>(worldPos.y);
                    vertex.position[2] = static_cast<float>(worldPos.z);

                    // UV 가져오기
                    if (mesh->vertex_uv.exists) {
                        ufbx_vec2 uv = ufbx_get_vertex_vec2(&mesh->vertex_uv, index);
                        vertex.texCoord[0] = static_cast<float>(uv.x);
                        vertex.texCoord[1] = static_cast<float>(uv.y); // FBX 포맷은 기본적으로 좌측 하단(Bottom-Left) 원점이므로 그대로 사용합니다.
                    } else {
                        vertex.texCoord[0] = 0.0f;
                        vertex.texCoord[1] = 0.0f;
                    }

                    // 기본 색상을 하얗게 설정
                    vertex.color[0] = 1.0f;
                    vertex.color[1] = 1.0f;
                    vertex.color[2] = 1.0f;

                    outVertices.push_back(vertex);
                    outIndices.push_back(currentIndex++);
                }
            }
        }

        ufbx_free_scene(scene);
        return true;
    }
}
