#include "Graphics/Mesh.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <sstream>

#include "Graphics/Scene.h"

#include <fastgltf/core.hpp>
#include <fastgltf/math.hpp>
#include <fastgltf/tools.hpp>
#include <fastgltf/types.hpp>
#include <ufbx.h>

template<class... Ts> struct dy_gltf_visitor : Ts... { using Ts::operator()...; };
template<class... Ts> dy_gltf_visitor(Ts...) -> dy_gltf_visitor<Ts...>;

namespace dy::Graphics
{
	namespace
	{
		template <typename T>
		bool IsValidIndex(int index, const std::vector<T>& values)
		{
			return index >= 0 && static_cast<size_t>(index) < values.size();
		}

		[[nodiscard]] Math::float3 BuildFallbackTangent(const Math::float3& normal)
		{
			const Math::float3 up = std::fabs(normal.z) < 0.999f
				? Math::float3(0.0f, 0.0f, 1.0f)
				: Math::float3(0.0f, 1.0f, 0.0f);
			return NormalizeOr(Cross(up, normal), Math::float3(1.0f, 0.0f, 0.0f));
		}

		void CalculateTangents(MeshData& data)
		{
			std::vector<Math::float3> tangents(data.vertices.size(), Math::float3(0.0f, 0.0f, 0.0f));
			std::vector<Math::float3> bitangents(data.vertices.size(), Math::float3(0.0f, 0.0f, 0.0f));

			for(size_t i = 0; i + 2 < data.indices.size(); i += 3)
			{
				const uint32_t i0 = data.indices[i + 0];
				const uint32_t i1 = data.indices[i + 1];
				const uint32_t i2 = data.indices[i + 2];
				if(i0 >= data.vertices.size() || i1 >= data.vertices.size() || i2 >= data.vertices.size()) continue;

				const Vertex& v0 = data.vertices[i0];
				const Vertex& v1 = data.vertices[i1];
				const Vertex& v2 = data.vertices[i2];

				const Math::float3 edge1 = v1.position - v0.position;
				const Math::float3 edge2 = v2.position - v0.position;
				const float du1 = v1.uv.x - v0.uv.x;
				const float dv1 = v1.uv.y - v0.uv.y;
				const float du2 = v2.uv.x - v0.uv.x;
				const float dv2 = v2.uv.y - v0.uv.y;
				const float determinant = du1 * dv2 - du2 * dv1;
				if(std::fabs(determinant) <= 1.0e-8f) continue;

				const float invDeterminant = 1.0f / determinant;
				const Math::float3 tangent = ((edge1 * dv2) - (edge2 * dv1)) * invDeterminant;
				const Math::float3 bitangent = ((edge2 * du1) - (edge1 * du2)) * invDeterminant;

				tangents[i0] = tangents[i0] + tangent;
				tangents[i1] = tangents[i1] + tangent;
				tangents[i2] = tangents[i2] + tangent;
				bitangents[i0] = bitangents[i0] + bitangent;
				bitangents[i1] = bitangents[i1] + bitangent;
				bitangents[i2] = bitangents[i2] + bitangent;
			}

			for(size_t i = 0; i < data.vertices.size(); ++i)
			{
				Vertex& vertex = data.vertices[i];
				const Math::float3 normal = NormalizeOr(vertex.normal, Math::float3(0.0f, 0.0f, 1.0f));
				const Math::float3 rawTangent = tangents[i];
				const Math::float3 orthogonalTangent = rawTangent - (normal * Dot(normal, rawTangent));
				const Math::float3 tangent = NormalizeOr(orthogonalTangent, BuildFallbackTangent(normal));
				const float handedness = Dot(Cross(normal, tangent), bitangents[i]) < 0.0f ? -1.0f : 1.0f;
				vertex.normal = normal;
				vertex.tangent = Math::float4(tangent.x, tangent.y, tangent.z, handedness);
			}
		}

		[[nodiscard]] uint32_t EnsureDefaultMaterial(ModelData& model)
		{
			if(model.materials.empty())
			{
				model.materials.push_back({});
			}
			return 0u;
		}

		void SetTexturePath(ModelMaterialInfo& material, MaterialTextureKind kind, std::string path)
		{
			const uint32_t index = static_cast<uint32_t>(kind);
			material.texturePaths[index] = std::move(path);
			material.hasTexture[index] = !material.texturePaths[index].empty();
		}

		struct ObjMaterialRecord
		{
			ModelMaterialInfo material;
			uint32_t index = 0u;
		};

		void LoadObjMaterialLibrary(
			const std::filesystem::path& objPath,
			const std::string& libraryName,
			ModelData& model,
			std::map<std::string, ObjMaterialRecord>& materials)
		{
			const std::filesystem::path libraryPath = objPath.parent_path() / libraryName;
			std::ifstream file(libraryPath);
			if(!file.is_open()) return;

			std::string currentName;
			std::string line;
			while(std::getline(file, line))
			{
				std::istringstream ss(line);
				std::string token;
				ss >> token;

				if(token == "newmtl")
				{
					ss >> currentName;
					ModelMaterialInfo info = {};
					info.name = currentName;
					const uint32_t index = static_cast<uint32_t>(model.materials.size());
					model.materials.push_back(info);
					materials[currentName] = ObjMaterialRecord{ info, index };
				}
				else if(token == "Kd" && !currentName.empty())
				{
					float r = 1.0f;
					float g = 1.0f;
					float b = 1.0f;
					ss >> r >> g >> b;
					ObjMaterialRecord& record = materials[currentName];
					record.material.material.baseColor = Math::float4(r, g, b, record.material.material.baseColor.w);
					record.material.hasBaseColor = true;
					model.materials[record.index] = record.material;
				}
				else if(token == "map_Kd" && !currentName.empty())
				{
					std::string textureFile;
					ss >> textureFile;
					if(!textureFile.empty())
					{
						ObjMaterialRecord& record = materials[currentName];
						SetTexturePath(record.material, MaterialTextureKind::BaseColor, (libraryPath.parent_path() / textureFile).string());
						model.materials[record.index] = record.material;
					}
				}
			}
		}

		struct OBJIndex
		{
			int v = 0;
			int vt = 0;
			int vn = 0;
			uint32_t materialIndex = 0;

			bool operator<(const OBJIndex& other) const
			{
				if(v != other.v) return v < other.v;
				if(vt != other.vt) return vt < other.vt;
				if(vn != other.vn) return vn < other.vn;
				return materialIndex < other.materialIndex;
			}
		};

		[[nodiscard]] bool LoadObjModel(const std::string& path, ModelData& outModel, const ModelLoadOptions& options)
		{
			std::ifstream file(path);
			if(!file.is_open()) return false;

			outModel = {};
			const std::filesystem::path objPath(path);
			std::vector<Math::float3> positions;
			std::vector<Math::float2> uvs;
			std::vector<Math::float3> normals;
			std::map<std::string, ObjMaterialRecord> materials;
			std::map<uint32_t, ModelMesh> meshesByMaterial;
			std::map<uint32_t, std::map<OBJIndex, uint32_t>> uniqueVerticesByMaterial;
			uint32_t currentMaterialIndex = EnsureDefaultMaterial(outModel);
			Math::float4 currentColor = outModel.materials[currentMaterialIndex].material.baseColor;

			auto getMesh = [&]() -> ModelMesh& {
				ModelMesh& mesh = meshesByMaterial[currentMaterialIndex];
				if(mesh.name.empty())
				{
					const std::string materialName = currentMaterialIndex < outModel.materials.size()
						? outModel.materials[currentMaterialIndex].name
						: std::string{};
					mesh.name = materialName.empty()
						? objPath.stem().string() + "_" + std::to_string(currentMaterialIndex)
						: objPath.stem().string() + "_" + materialName;
					mesh.materialIndex = currentMaterialIndex;
				}
				return mesh;
			};

			std::string line;
			while(std::getline(file, line))
			{
				std::stringstream ss(line);
				std::string prefix;
				ss >> prefix;

				if(prefix == "v")
				{
					Math::float3 v;
					ss >> v.x >> v.y >> v.z;
					positions.push_back(v);
				}
				else if(prefix == "mtllib")
				{
					std::string libraryName;
					ss >> libraryName;
					if(!libraryName.empty()) LoadObjMaterialLibrary(objPath, libraryName, outModel, materials);
				}
				else if(prefix == "usemtl")
				{
					std::string materialName;
					ss >> materialName;
					const auto it = materials.find(materialName);
					if(it != materials.end())
					{
						currentMaterialIndex = it->second.index;
						currentColor = it->second.material.material.baseColor;
					}
				}
				else if(prefix == "vt")
				{
					Math::float2 vt;
					ss >> vt.x >> vt.y;
					if(options.flipV) vt.y = 1.0f - vt.y;
					uvs.push_back(vt);
				}
				else if(prefix == "vn")
				{
					Math::float3 vn;
					ss >> vn.x >> vn.y >> vn.z;
					normals.push_back(vn);
				}
				else if(prefix == "f")
				{
					ModelMesh& mesh = getMesh();
					std::map<OBJIndex, uint32_t>& uniqueVertices = uniqueVerticesByMaterial[currentMaterialIndex];
					std::string vertexStr;
					std::vector<uint32_t> faceIndices;
					while(ss >> vertexStr)
					{
						int vIdx = 0;
						int vtIdx = 0;
						int vnIdx = 0;
						for(char& c : vertexStr) if(c == '/') c = ' ';
						std::stringstream vss(vertexStr);

						vss >> vIdx;
						if(vertexStr.find("  ") != std::string::npos) {
							vss >> vnIdx;
						} else {
							vss >> vtIdx >> vnIdx;
						}

						OBJIndex idx = { vIdx - 1, vtIdx - 1, vnIdx - 1, currentMaterialIndex };
						if(uniqueVertices.count(idx) == 0)
						{
							uniqueVertices[idx] = static_cast<uint32_t>(mesh.mesh.vertices.size());
							Vertex v = {};
							v.position = IsValidIndex(idx.v, positions) ? positions[static_cast<size_t>(idx.v)] : Math::float3(0.0f, 0.0f, 0.0f);
							v.uv = IsValidIndex(idx.vt, uvs) ? uvs[static_cast<size_t>(idx.vt)] : Math::float2(0.0f, 0.0f);
							v.normal = IsValidIndex(idx.vn, normals) ? normals[static_cast<size_t>(idx.vn)] : Math::float3(0.0f, 0.0f, 1.0f);
							v.color = currentColor;
							mesh.mesh.vertices.push_back(v);
						}
						faceIndices.push_back(uniqueVertices[idx]);
					}

					for(size_t i = 1; i + 1 < faceIndices.size(); ++i)
					{
						mesh.mesh.indices.push_back(faceIndices[0]);
						mesh.mesh.indices.push_back(faceIndices[i]);
						mesh.mesh.indices.push_back(faceIndices[i + 1]);
					}
				}
			}

			for(auto& [materialIndex, mesh] : meshesByMaterial)
			{
				(void)materialIndex;
				if(mesh.mesh.indices.empty()) continue;
				CalculateTangents(mesh.mesh);
				outModel.meshes.push_back(std::move(mesh));
			}
			return !outModel.meshes.empty();
		}

		void ApplyGltfMaterialTexture(
			const std::filesystem::path& basePath,
			const fastgltf::Asset& gltf,
			const fastgltf::TextureInfo& textureInfo,
			ModelMaterialInfo& material,
			MaterialTextureKind kind)
		{
			const fastgltf::Texture& texture = gltf.textures[textureInfo.textureIndex];
			if(!texture.imageIndex.has_value()) return;

			const fastgltf::Image& image = gltf.images[texture.imageIndex.value()];
			std::visit(dy_gltf_visitor{
				[&](const fastgltf::sources::URI& filePath) {
					SetTexturePath(material, kind, (basePath / filePath.uri.fspath()).string());
				},
				[&](const auto&) {}
			}, image.data);
		}

		[[nodiscard]] bool LoadGltfModel(const std::string& filepath, ModelData& outModel, const ModelLoadOptions& options)
		{
			auto data = fastgltf::GltfDataBuffer::FromPath(filepath);
			if(data.error() != fastgltf::Error::None)
			{
				std::cerr << "Failed to load glTF file: " << filepath << std::endl;
				return false;
			}

			fastgltf::Parser parser;
			const std::filesystem::path basePath = std::filesystem::path(filepath).parent_path();
			auto asset = parser.loadGltf(data.get(), basePath, fastgltf::Options::LoadExternalBuffers);
			if(asset.error() != fastgltf::Error::None)
			{
				std::cerr << "Failed to parse glTF: " << fastgltf::getErrorMessage(asset.error()) << std::endl;
				return false;
			}

			outModel = {};
			fastgltf::Asset& gltf = asset.get();
			outModel.materials.reserve(gltf.materials.size());
			for(const fastgltf::Material& source : gltf.materials)
			{
				ModelMaterialInfo material = {};
				material.name = source.name.c_str();
				material.material.baseColor = Math::float4(
					source.pbrData.baseColorFactor[0],
					source.pbrData.baseColorFactor[1],
					source.pbrData.baseColorFactor[2],
					source.pbrData.baseColorFactor[3]);
				material.material.metallicFactor = source.pbrData.metallicFactor;
				material.material.roughnessFactor = source.pbrData.roughnessFactor;
				material.material.emissiveColor = Math::float3(
					source.emissiveFactor[0],
					source.emissiveFactor[1],
					source.emissiveFactor[2]);
				material.hasBaseColor = true;
				if(source.pbrData.baseColorTexture.has_value()) ApplyGltfMaterialTexture(basePath, gltf, source.pbrData.baseColorTexture.value(), material, MaterialTextureKind::BaseColor);
				if(source.pbrData.metallicRoughnessTexture.has_value()) ApplyGltfMaterialTexture(basePath, gltf, source.pbrData.metallicRoughnessTexture.value(), material, MaterialTextureKind::MetallicRoughness);
				if(source.normalTexture.has_value()) ApplyGltfMaterialTexture(basePath, gltf, source.normalTexture.value(), material, MaterialTextureKind::Normal);
				if(source.occlusionTexture.has_value()) ApplyGltfMaterialTexture(basePath, gltf, source.occlusionTexture.value(), material, MaterialTextureKind::Occlusion);
				if(source.emissiveTexture.has_value()) ApplyGltfMaterialTexture(basePath, gltf, source.emissiveTexture.value(), material, MaterialTextureKind::Emissive);
				outModel.materials.push_back(std::move(material));
			}
			[[maybe_unused]] const uint32_t defaultMaterialIndex = EnsureDefaultMaterial(outModel);

			std::function<void(size_t, fastgltf::math::mat<float, 4, 4>)> processNode =
				[&](size_t nodeIndex, fastgltf::math::mat<float, 4, 4> parentMatrix)
			{
				const fastgltf::Node& node = gltf.nodes[nodeIndex];
				const fastgltf::math::mat<float, 4, 4> globalMatrix = parentMatrix * fastgltf::getTransformMatrix(node);

				if(node.meshIndex.has_value())
				{
					const fastgltf::Mesh& sourceMesh = gltf.meshes[node.meshIndex.value()];
					for(const fastgltf::Primitive& primitive : sourceMesh.primitives)
					{
						auto* posAttr = primitive.findAttribute("POSITION");
						if(posAttr == nullptr) continue;

						const fastgltf::Accessor& posAccessor = gltf.accessors[posAttr->accessorIndex];
						ModelMesh mesh = {};
						mesh.name = sourceMesh.name.c_str();
						mesh.materialIndex = primitive.materialIndex.has_value() ? static_cast<uint32_t>(primitive.materialIndex.value()) : 0u;
						mesh.mesh.vertices.resize(posAccessor.count);

						fastgltf::iterateAccessorWithIndex<fastgltf::math::fvec3>(gltf, posAccessor, [&](fastgltf::math::fvec3 pos, size_t idx) {
							const fastgltf::math::fvec4 p(pos.x(), pos.y(), pos.z(), 1.0f);
							const fastgltf::math::fvec4 transformedPos = globalMatrix * p;
							Vertex& vertex = mesh.mesh.vertices[idx];
							vertex.position = Math::float3(transformedPos.x(), transformedPos.y(), transformedPos.z());
							vertex.uv = Math::float2(0.0f, 0.0f);
							vertex.normal = Math::float3(0.0f, 1.0f, 0.0f);
						});

						if(auto* normalAttr = primitive.findAttribute("NORMAL"))
						{
							const fastgltf::Accessor& normalAcc = gltf.accessors[normalAttr->accessorIndex];
							fastgltf::iterateAccessorWithIndex<fastgltf::math::fvec3>(gltf, normalAcc, [&](fastgltf::math::fvec3 normal, size_t idx) {
								const fastgltf::math::fvec4 n(normal.x(), normal.y(), normal.z(), 0.0f);
								const fastgltf::math::fvec4 transformedNormal = globalMatrix * n;
								mesh.mesh.vertices[idx].normal = NormalizeOr(
									Math::float3(transformedNormal.x(), transformedNormal.y(), transformedNormal.z()),
									Math::float3(0.0f, 1.0f, 0.0f));
							});
						}

						if(auto* uvAttr = primitive.findAttribute("TEXCOORD_0"))
						{
							const fastgltf::Accessor& uvAcc = gltf.accessors[uvAttr->accessorIndex];
							fastgltf::iterateAccessorWithIndex<fastgltf::math::fvec2>(gltf, uvAcc, [&](fastgltf::math::fvec2 uv, size_t idx) {
								mesh.mesh.vertices[idx].uv = Math::float2(uv.x(), options.flipV ? 1.0f - uv.y() : uv.y());
							});
						}

						if(primitive.indicesAccessor.has_value())
						{
							const fastgltf::Accessor& idxAccessor = gltf.accessors[primitive.indicesAccessor.value()];
							fastgltf::iterateAccessor<std::uint32_t>(gltf, idxAccessor, [&](std::uint32_t idx) {
								mesh.mesh.indices.push_back(idx);
							});
						}
						else
						{
							for(size_t i = 0; i < posAccessor.count; ++i) mesh.mesh.indices.push_back(static_cast<uint32_t>(i));
						}

						CalculateTangents(mesh.mesh);
						outModel.meshes.push_back(std::move(mesh));
					}
				}

				for(const size_t childIndex : node.children) processNode(childIndex, globalMatrix);
			};

			const fastgltf::Scene* scene = gltf.defaultScene.has_value() ? &gltf.scenes[*gltf.defaultScene] : &gltf.scenes[0];
			fastgltf::math::mat<float, 4, 4> rotationMatrix(1.0f);
			rotationMatrix.col(0).x() = -1.0f;
			rotationMatrix.col(2).z() = -1.0f;
			for(const size_t nodeIndex : scene->nodeIndices) processNode(nodeIndex, rotationMatrix);
			return !outModel.meshes.empty();
		}

		[[nodiscard]] bool LoadFbxModel(const std::string& filepath, ModelData& outModel, const ModelLoadOptions& options)
		{
			(void)options;
			ufbx_load_opts opts = {};
			opts.load_external_files = false;

			ufbx_error error;
			ufbx_scene* scene = ufbx_load_file(filepath.c_str(), &opts, &error);
			if(scene == nullptr)
			{
				std::cerr << "Failed to load FBX file: " << filepath << "\nReason: " << error.info << std::endl;
				return false;
			}



			outModel = {};
			outModel.materials.reserve(scene->materials.count);
			std::map<const ufbx_material*, uint32_t> materialIndices;
			for(size_t i = 0; i < scene->materials.count; ++i)
			{
				ufbx_material* mat = scene->materials.data[i];
				ModelMaterialInfo material = {};
				material.name = mat->name.data != nullptr ? mat->name.data : "";
				material.material.baseColor = Math::float4(
					static_cast<float>(mat->pbr.base_color.value_vec3.x),
					static_cast<float>(mat->pbr.base_color.value_vec3.y),
					static_cast<float>(mat->pbr.base_color.value_vec3.z),
					1.0f);
				material.material.metallicFactor = static_cast<float>(mat->pbr.metalness.value_real);
				material.material.roughnessFactor = static_cast<float>(mat->pbr.roughness.value_real);
				material.hasBaseColor = true;

				ufbx_texture* baseColorTexture = mat->pbr.base_color.texture != nullptr ? mat->pbr.base_color.texture : mat->fbx.diffuse_color.texture;
				if(baseColorTexture != nullptr)
				{
					// FBX 의 텍스처 경로는 절대(filename)·상대(relative_filename) 어느 쪽만 있거나
					// 저장 경로가 실제와 어긋날 수 있다. 여러 후보 중 실제 존재하는 파일을 고른다.
					const std::filesystem::path fbxDir = std::filesystem::path(filepath).parent_path();
					std::vector<std::filesystem::path> candidates;
					if(baseColorTexture->relative_filename.length > 0)
					{
						const std::filesystem::path rel = baseColorTexture->relative_filename.data;
						candidates.push_back(fbxDir / rel);
						const std::filesystem::path base = rel.filename();
						candidates.push_back(fbxDir / base);
						candidates.push_back(fbxDir / "textures" / base);
					}
					if(baseColorTexture->filename.length > 0)
					{
						candidates.push_back(std::filesystem::path(baseColorTexture->filename.data));
					}
					std::error_code ec;
					for(const std::filesystem::path& candidate : candidates)
					{
						if(std::filesystem::exists(candidate, ec))
						{
							SetTexturePath(material, MaterialTextureKind::BaseColor, candidate.string());
							break;
						}
					}
				}
				else
				{
					// 폴백 로직: 디렉토리 내에서 적절한 이미지 파일을 직접 탐색
					const std::filesystem::path fbxDir = std::filesystem::path(filepath).parent_path();
					std::vector<std::filesystem::path> searchDirs = { fbxDir, fbxDir / "textures" };
					std::vector<std::filesystem::path> imageFiles;
					std::error_code ec;

					for(const auto& dir : searchDirs)
					{
						if(std::filesystem::exists(dir, ec) && std::filesystem::is_directory(dir, ec))
						{
							for(const auto& entry : std::filesystem::directory_iterator(dir, ec))
							{
								if(entry.is_regular_file(ec))
								{
									std::string ext = entry.path().extension().string();
									std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
									if(ext == ".png" || ext == ".jpg" || ext == ".jpeg")
									{
										imageFiles.push_back(entry.path());
									}
								}
							}
						}
					}

					std::filesystem::path selectedFallback;
					std::string matNameLower = material.name;
					std::transform(matNameLower.begin(), matNameLower.end(), matNameLower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

					for(const auto& imgPath : imageFiles)
					{
						std::string filenameLower = imgPath.filename().string();
						std::transform(filenameLower.begin(), filenameLower.end(), filenameLower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

						if(!matNameLower.empty() && filenameLower.find(matNameLower) != std::string::npos)
						{
							selectedFallback = imgPath;
							break;
						}
					}

					if(selectedFallback.empty() && !imageFiles.empty())
					{
						for(const auto& imgPath : imageFiles)
						{
							std::string filenameLower = imgPath.filename().string();
							std::transform(filenameLower.begin(), filenameLower.end(), filenameLower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
							if(filenameLower.find("base") != std::string::npos || filenameLower.find("diffuse") != std::string::npos || filenameLower.find("color") != std::string::npos)
							{
								selectedFallback = imgPath;
								break;
							}
						}
						if(selectedFallback.empty())
						{
							selectedFallback = imageFiles.front();
						}
					}

					if(!selectedFallback.empty())
					{
						std::cout << "[FBX Fallback] Found texture fallback: " << selectedFallback.string() << " for material: " << material.name << std::endl;
						SetTexturePath(material, MaterialTextureKind::BaseColor, selectedFallback.string());
					}
				}
				materialIndices[mat] = static_cast<uint32_t>(outModel.materials.size());
				outModel.materials.push_back(std::move(material));
			}
			[[maybe_unused]] const uint32_t defaultMaterialIndex = EnsureDefaultMaterial(outModel);

			auto resolveMaterialIndex = [&](ufbx_node* node, ufbx_mesh* sourceMesh, uint32_t localMaterialIndex) -> uint32_t {
				ufbx_material* material = nullptr;
				if(localMaterialIndex < node->materials.count)
				{
					material = node->materials.data[localMaterialIndex];
				}
				else if(localMaterialIndex < sourceMesh->materials.count)
				{
					material = sourceMesh->materials.data[localMaterialIndex];
				}
				const auto it = materialIndices.find(material);
				return it != materialIndices.end() ? it->second : defaultMaterialIndex;
			};

			for(size_t ni = 0; ni < scene->nodes.count; ++ni)
			{
				ufbx_node* node = scene->nodes.data[ni];
				if(node->mesh == nullptr) continue;

				ufbx_mesh* sourceMesh = node->mesh;
				std::map<uint32_t, ModelMesh> meshesByMaterial;
				const std::string nodeName = node->name.data != nullptr ? node->name.data : "";

				auto getMesh = [&](uint32_t materialIndex) -> ModelMesh& {
					ModelMesh& mesh = meshesByMaterial[materialIndex];
					if(mesh.name.empty())
					{
						const std::string materialName = materialIndex < outModel.materials.size()
							? outModel.materials[materialIndex].name
							: std::string{};
						mesh.name = materialName.empty()
							? nodeName + "_" + std::to_string(materialIndex)
							: nodeName + "_" + materialName;
						mesh.materialIndex = materialIndex;
					}
					return mesh;
				};

				for(size_t fi = 0; fi < sourceMesh->faces.count; ++fi)
				{
					ufbx_face face = sourceMesh->faces.data[fi];
					if(face.num_indices < 3) continue;

					const uint32_t localMaterialIndex = fi < sourceMesh->face_material.count
						? sourceMesh->face_material.data[fi]
						: 0u;
					ModelMesh& mesh = getMesh(resolveMaterialIndex(node, sourceMesh, localMaterialIndex));
					std::vector<uint32_t> triIndices((face.num_indices - 2u) * 3u);
					const size_t numTris = ufbx_triangulate_face(triIndices.data(), triIndices.size(), sourceMesh, face);

					for(size_t ti = 0; ti < numTris * 3u; ++ti)
					{
						const uint32_t index = triIndices[ti];
						Vertex vertex = {};

						const ufbx_vec3 localPos = ufbx_get_vertex_vec3(&sourceMesh->vertex_position, index);
						const ufbx_vec3 worldPos = ufbx_transform_position(&node->geometry_to_world, localPos);
						vertex.position = Math::float3(static_cast<float>(worldPos.x), static_cast<float>(worldPos.y), static_cast<float>(worldPos.z));

						if(sourceMesh->vertex_normal.exists)
						{
							const ufbx_vec3 localNormal = ufbx_get_vertex_vec3(&sourceMesh->vertex_normal, index);
							const ufbx_vec3 worldNormal = ufbx_transform_direction(&node->geometry_to_world, localNormal);
							vertex.normal = NormalizeOr(
								Math::float3(static_cast<float>(worldNormal.x), static_cast<float>(worldNormal.y), static_cast<float>(worldNormal.z)),
								Math::float3(0.0f, 1.0f, 0.0f));
						}
						else
						{
							vertex.normal = Math::float3(0.0f, 1.0f, 0.0f);
						}

						if(sourceMesh->vertex_uv.exists)
						{
							const ufbx_vec2 uv = ufbx_get_vertex_vec2(&sourceMesh->vertex_uv, index);
							// FBX 포맷은 기본적으로 텍스처 좌표계 Y축(V)이 아래에서 위로 향하지만, 
							// 통합 엔진에서는 텍스처 이미지의 수직 반전을 수행하지 않으므로 UV의 Y축을 반전시켜야 렌더링 시 정상 출력됩니다.
							vertex.uv = Math::float2(static_cast<float>(uv.x), 1.0f - static_cast<float>(uv.y));
						}

						mesh.mesh.indices.push_back(static_cast<uint32_t>(mesh.mesh.vertices.size()));
						mesh.mesh.vertices.push_back(vertex);
					}
				}

				for(auto& [materialIndex, mesh] : meshesByMaterial)
				{
					(void)materialIndex;
					if(mesh.mesh.indices.empty()) continue;
					CalculateTangents(mesh.mesh);
					outModel.meshes.push_back(std::move(mesh));
				}
			}

			ufbx_free_scene(scene);
			return !outModel.meshes.empty();
		}

		struct ModelBounds
		{
			Math::float3 min;
			Math::float3 max;
			bool valid = false;
		};

		void IncludePoint(ModelBounds& bounds, const Math::float3& point)
		{
			if(!bounds.valid)
			{
				bounds.min = point;
				bounds.max = point;
				bounds.valid = true;
				return;
			}
			bounds.min.x = std::min(bounds.min.x, point.x);
			bounds.min.y = std::min(bounds.min.y, point.y);
			bounds.min.z = std::min(bounds.min.z, point.z);
			bounds.max.x = std::max(bounds.max.x, point.x);
			bounds.max.y = std::max(bounds.max.y, point.y);
			bounds.max.z = std::max(bounds.max.z, point.z);
		}

		[[nodiscard]] ModelBounds ComputeModelBounds(const ModelData& model)
		{
			ModelBounds bounds = {};
			for(const ModelMesh& mesh : model.meshes)
			{
				for(const Vertex& vertex : mesh.mesh.vertices)
				{
					IncludePoint(bounds, vertex.position);
				}
			}
			return bounds;
		}

		[[nodiscard]] Math::float4x4 BuildModelSceneTransform(const ModelData& model, const ModelSceneDesc& desc)
		{
			Math::float4x4 transform = Math::Translation(desc.position);
			if(desc.yUpToZUp)
			{
				// Y-up(+Z forward) → Z-up. RotationX(90°)만 하면 forward 가 world -Y 가 되어
				// (+Y 쪽 카메라가 모델 뒤통수를 봄) 180° yaw 를 더해 forward 를 +Y 로 돌린다.
				transform = transform * Math::RotationZ(3.14159265f) * Math::RotationX(1.5707963f);
			}
			if(desc.normalize)
			{
				const ModelBounds bounds = ComputeModelBounds(model);
				if(bounds.valid)
				{
					const Math::float3 center(
						(bounds.min.x + bounds.max.x) * 0.5f,
						(bounds.min.y + bounds.max.y) * 0.5f,
						(bounds.min.z + bounds.max.z) * 0.5f);
					const float width = bounds.max.x - bounds.min.x;
					const float height = bounds.max.y - bounds.min.y;
					const float depth = bounds.max.z - bounds.min.z;
					const float largestAxis = std::max(std::max(width, height), depth);
					const float scale = largestAxis > 0.0001f ? desc.normalizedSize / largestAxis : 1.0f;
					transform = transform * Math::Scaling(scale) * Math::Translation(Math::float3(-center.x, -center.y, -center.z));
				}
			}
			return transform * desc.transform;
		}

		[[nodiscard]] TextureID CreateTextureIfPresent(Scene& scene, const ModelMaterialInfo& material, MaterialTextureKind kind)
		{
			const uint32_t slot = static_cast<uint32_t>(kind);
			return material.hasTexture[slot] ? scene.CreateTexture(material.texturePaths[slot]) : TextureID::Invalid;
		}

		[[nodiscard]] MaterialID CreateSceneMaterial(Scene& scene, const ModelMaterialInfo& source)
		{
			MaterialDesc material = source.material;
			material.baseColorTexture = CreateTextureIfPresent(scene, source, MaterialTextureKind::BaseColor);
			material.metallicRoughnessTexture = CreateTextureIfPresent(scene, source, MaterialTextureKind::MetallicRoughness);
			material.normalTexture = CreateTextureIfPresent(scene, source, MaterialTextureKind::Normal);
			material.occlusionTexture = CreateTextureIfPresent(scene, source, MaterialTextureKind::Occlusion);
			material.emissiveTexture = CreateTextureIfPresent(scene, source, MaterialTextureKind::Emissive);
			return scene.CreateMaterial(material);
		}
	}

	bool ModelLoader::Load(const std::string& path, ModelData& outModel, const ModelLoadOptions& options)
	{
		return LoadModel(path, outModel, options);
	}

	bool ModelLoader::LoadMesh(const std::string& path, MeshData& outMesh, ModelMaterialInfo* outMaterial, const ModelLoadOptions& options)
	{
		return Graphics::LoadMesh(path, outMesh, outMaterial, options);
	}

	bool ModelLoader::LoadMesh(const std::string& path, MeshData& outMesh, std::string* outBaseColorTexturePath, const ModelLoadOptions& options)
	{
		return Graphics::LoadMesh(path, outMesh, outBaseColorTexturePath, options);
	}

	bool LoadModel(const std::string& path, ModelData& outModel, const ModelLoadOptions& options)
	{
		const std::string extension = std::filesystem::path(path).extension().string();
		std::string lowerExtension = extension;
		std::transform(lowerExtension.begin(), lowerExtension.end(), lowerExtension.begin(), [](unsigned char c) {
			return static_cast<char>(std::tolower(c));
		});

		if(lowerExtension == ".obj") return LoadObjModel(path, outModel, options);
		if(lowerExtension == ".gltf" || lowerExtension == ".glb") return LoadGltfModel(path, outModel, options);
		if(lowerExtension == ".fbx") return LoadFbxModel(path, outModel, options);
		return false;
	}

	bool LoadMesh(const std::string& path, MeshData& outMesh, ModelMaterialInfo* outMaterial, const ModelLoadOptions& options)
	{
		ModelData model = {};
		if(!LoadModel(path, model, options)) return false;
		outMesh = MergeModelMeshes(model);
		if(outMaterial != nullptr)
		{
			*outMaterial = !model.materials.empty() ? model.materials.front() : ModelMaterialInfo{};
		}
		return true;
	}

	bool LoadMesh(const std::string& path, MeshData& outMesh, std::string* outBaseColorTexturePath, const ModelLoadOptions& options)
	{
		ModelMaterialInfo material = {};
		if(!LoadMesh(path, outMesh, &material, options)) return false;
		if(outBaseColorTexturePath != nullptr)
		{
			const uint32_t slot = static_cast<uint32_t>(MaterialTextureKind::BaseColor);
			*outBaseColorTexturePath = material.hasTexture[slot] ? material.texturePaths[slot] : std::string{};
		}
		return true;
	}

	bool AddModelToScene(Scene& scene, const ModelSceneDesc& desc)
	{
		ModelData model = {};
		if(!LoadModel(desc.path, model, desc.loadOptions)) return false;

		std::vector<MaterialID> materials;
		materials.reserve(model.materials.size());
		for(const ModelMaterialInfo& material : model.materials)
		{
			materials.push_back(CreateSceneMaterial(scene, material));
		}
		if(materials.empty())
		{
			materials.push_back(scene.CreateMaterial(MaterialDesc{}));
		}

		const Math::float4x4 transform = BuildModelSceneTransform(model, desc);
		bool createdAny = false;
		for(const ModelMesh& modelMesh : model.meshes)
		{
			if(modelMesh.mesh.vertices.empty() || modelMesh.mesh.indices.empty()) continue;

			const MeshID mesh = scene.CreateMesh(modelMesh.mesh);
			const MaterialID material = modelMesh.materialIndex < materials.size()
				? materials[modelMesh.materialIndex]
				: materials.front();
			scene.CreateEntity(mesh, material, transform, desc.renderFlags);
			createdAny = true;
		}
		return createdAny;
	}

	bool AddModelToScene(Scene& scene, const std::string& path, const Math::float3& position)
	{
		ModelSceneDesc desc = {};
		desc.path = path;
		desc.position = position;
		return AddModelToScene(scene, desc);
	}

	MeshData MergeModelMeshes(const ModelData& model)
	{
		MeshData merged = {};
		for(const ModelMesh& sourceMesh : model.meshes)
		{
			const uint32_t vertexOffset = static_cast<uint32_t>(merged.vertices.size());
			merged.vertices.insert(merged.vertices.end(), sourceMesh.mesh.vertices.begin(), sourceMesh.mesh.vertices.end());
			for(const uint32_t index : sourceMesh.mesh.indices)
			{
				merged.indices.push_back(vertexOffset + index);
			}
		}
		return merged;
	}

	MeshData CreateCubeMesh(float size)
	{
		const float h = std::max(size, 0.0f) * 0.5f;
		MeshData mesh = {};
		mesh.vertices.reserve(24u);
		mesh.indices.reserve(36u);

		auto addFace = [&](const Math::float3& a, const Math::float3& b, const Math::float3& c, const Math::float3& d, const Math::float3& normal, const Math::float4& tangent)
		{
			const uint32_t base = static_cast<uint32_t>(mesh.vertices.size());
			Vertex v0 = {};
			Vertex v1 = {};
			Vertex v2 = {};
			Vertex v3 = {};
			v0.position = a;
			v1.position = b;
			v2.position = c;
			v3.position = d;
			v0.normal = normal;
			v1.normal = normal;
			v2.normal = normal;
			v3.normal = normal;
			v0.tangent = tangent;
			v1.tangent = tangent;
			v2.tangent = tangent;
			v3.tangent = tangent;
			v0.uv = Math::float2(0.0f, 0.0f);
			v1.uv = Math::float2(1.0f, 0.0f);
			v2.uv = Math::float2(1.0f, 1.0f);
			v3.uv = Math::float2(0.0f, 1.0f);
			mesh.vertices.push_back(v0);
			mesh.vertices.push_back(v1);
			mesh.vertices.push_back(v2);
			mesh.vertices.push_back(v3);
			mesh.indices.push_back(base + 0u);
			mesh.indices.push_back(base + 1u);
			mesh.indices.push_back(base + 2u);
			mesh.indices.push_back(base + 0u);
			mesh.indices.push_back(base + 2u);
			mesh.indices.push_back(base + 3u);
		};

		addFace(Math::float3(-h, -h, h), Math::float3(h, -h, h), Math::float3(h, h, h), Math::float3(-h, h, h), Math::float3(0.0f, 0.0f, 1.0f), Math::float4(1.0f, 0.0f, 0.0f, 1.0f));
		addFace(Math::float3(h, -h, -h), Math::float3(-h, -h, -h), Math::float3(-h, h, -h), Math::float3(h, h, -h), Math::float3(0.0f, 0.0f, -1.0f), Math::float4(-1.0f, 0.0f, 0.0f, 1.0f));
		addFace(Math::float3(h, -h, h), Math::float3(h, -h, -h), Math::float3(h, h, -h), Math::float3(h, h, h), Math::float3(1.0f, 0.0f, 0.0f), Math::float4(0.0f, 0.0f, -1.0f, 1.0f));
		addFace(Math::float3(-h, -h, -h), Math::float3(-h, -h, h), Math::float3(-h, h, h), Math::float3(-h, h, -h), Math::float3(-1.0f, 0.0f, 0.0f), Math::float4(0.0f, 0.0f, 1.0f, 1.0f));
		addFace(Math::float3(-h, h, h), Math::float3(h, h, h), Math::float3(h, h, -h), Math::float3(-h, h, -h), Math::float3(0.0f, 1.0f, 0.0f), Math::float4(1.0f, 0.0f, 0.0f, 1.0f));
		addFace(Math::float3(-h, -h, -h), Math::float3(h, -h, -h), Math::float3(h, -h, h), Math::float3(-h, -h, h), Math::float3(0.0f, -1.0f, 0.0f), Math::float4(1.0f, 0.0f, 0.0f, 1.0f));
		return mesh;
	}

	const char* ToString(MaterialTextureKind kind)
	{
		switch(kind)
		{
		case MaterialTextureKind::BaseColor: return "BaseColor";
		case MaterialTextureKind::MetallicRoughness: return "MetallicRoughness";
		case MaterialTextureKind::Normal: return "Normal";
		case MaterialTextureKind::Occlusion: return "Occlusion";
		case MaterialTextureKind::Emissive: return "Emissive";
		default: return "Unknown";
		}
	}

}
