#include "Graphics/FBXLoader.h"

#include <ufbx.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace dy::Graphics
{
	namespace
	{
		dy::Math::float3 Add(const dy::Math::float3& a, const dy::Math::float3& b)
		{
			return dy::Math::float3(a.x + b.x, a.y + b.y, a.z + b.z);
		}

		dy::Math::float3 Subtract(const dy::Math::float3& a, const dy::Math::float3& b)
		{
			return dy::Math::float3(a.x - b.x, a.y - b.y, a.z - b.z);
		}

		dy::Math::float3 Scale(const dy::Math::float3& value, float scale)
		{
			return dy::Math::float3(value.x * scale, value.y * scale, value.z * scale);
		}

		float Dot(const dy::Math::float3& a, const dy::Math::float3& b)
		{
			return a.x * b.x + a.y * b.y + a.z * b.z;
		}

		dy::Math::float3 Cross(const dy::Math::float3& a, const dy::Math::float3& b)
		{
			return dy::Math::float3(
				a.y * b.z - a.z * b.y,
				a.z * b.x - a.x * b.z,
				a.x * b.y - a.y * b.x);
		}

		dy::Math::float3 NormalizeOr(const dy::Math::float3& value, const dy::Math::float3& fallback)
		{
			const float lengthSquared = Dot(value, value);
			if (lengthSquared <= 1.0e-8f) return fallback;
			const float invLength = 1.0f / std::sqrt(lengthSquared);
			return Scale(value, invLength);
		}

		dy::Math::float3 BuildFallbackTangent(const dy::Math::float3& normal)
		{
			const dy::Math::float3 up = std::fabs(normal.z) < 0.999f
				? dy::Math::float3(0.0f, 0.0f, 1.0f)
				: dy::Math::float3(0.0f, 1.0f, 0.0f);
			return NormalizeOr(Cross(up, normal), dy::Math::float3(1.0f, 0.0f, 0.0f));
		}

		void FillMissingNormals(MeshData& data)
		{
			std::vector<dy::Math::float3> accumulated(data.vertices.size(), dy::Math::float3(0.0f, 0.0f, 0.0f));

			for (size_t i = 0; i + 2 < data.indices.size(); i += 3)
			{
				const uint32_t i0 = data.indices[i + 0];
				const uint32_t i1 = data.indices[i + 1];
				const uint32_t i2 = data.indices[i + 2];
				if (i0 >= data.vertices.size() || i1 >= data.vertices.size() || i2 >= data.vertices.size()) continue;

				const dy::Math::float3 edge1 = Subtract(data.vertices[i1].position, data.vertices[i0].position);
				const dy::Math::float3 edge2 = Subtract(data.vertices[i2].position, data.vertices[i0].position);
				const dy::Math::float3 normal = NormalizeOr(Cross(edge1, edge2), dy::Math::float3(0.0f, 0.0f, 1.0f));

				accumulated[i0] = Add(accumulated[i0], normal);
				accumulated[i1] = Add(accumulated[i1], normal);
				accumulated[i2] = Add(accumulated[i2], normal);
			}

			for (size_t i = 0; i < data.vertices.size(); ++i)
			{
				if (Dot(data.vertices[i].normal, data.vertices[i].normal) > 1.0e-8f) continue;
				data.vertices[i].normal = NormalizeOr(accumulated[i], dy::Math::float3(0.0f, 0.0f, 1.0f));
			}
		}

		void CalculateTangents(MeshData& data)
		{
			std::vector<dy::Math::float3> tangents(data.vertices.size(), dy::Math::float3(0.0f, 0.0f, 0.0f));
			std::vector<dy::Math::float3> bitangents(data.vertices.size(), dy::Math::float3(0.0f, 0.0f, 0.0f));

			for (size_t i = 0; i + 2 < data.indices.size(); i += 3)
			{
				const uint32_t i0 = data.indices[i + 0];
				const uint32_t i1 = data.indices[i + 1];
				const uint32_t i2 = data.indices[i + 2];
				if (i0 >= data.vertices.size() || i1 >= data.vertices.size() || i2 >= data.vertices.size()) continue;

				const Vertex& v0 = data.vertices[i0];
				const Vertex& v1 = data.vertices[i1];
				const Vertex& v2 = data.vertices[i2];

				const dy::Math::float3 edge1 = Subtract(v1.position, v0.position);
				const dy::Math::float3 edge2 = Subtract(v2.position, v0.position);
				const float du1 = v1.uv.x - v0.uv.x;
				const float dv1 = v1.uv.y - v0.uv.y;
				const float du2 = v2.uv.x - v0.uv.x;
				const float dv2 = v2.uv.y - v0.uv.y;
				const float determinant = du1 * dv2 - du2 * dv1;
				if (std::fabs(determinant) <= 1.0e-8f) continue;

				const float invDeterminant = 1.0f / determinant;
				const dy::Math::float3 tangent = Scale(Subtract(Scale(edge1, dv2), Scale(edge2, dv1)), invDeterminant);
				const dy::Math::float3 bitangent = Scale(Subtract(Scale(edge2, du1), Scale(edge1, du2)), invDeterminant);

				tangents[i0] = Add(tangents[i0], tangent);
				tangents[i1] = Add(tangents[i1], tangent);
				tangents[i2] = Add(tangents[i2], tangent);
				bitangents[i0] = Add(bitangents[i0], bitangent);
				bitangents[i1] = Add(bitangents[i1], bitangent);
				bitangents[i2] = Add(bitangents[i2], bitangent);
			}

			for (size_t i = 0; i < data.vertices.size(); ++i)
			{
				Vertex& vertex = data.vertices[i];
				const dy::Math::float3 normal = NormalizeOr(vertex.normal, dy::Math::float3(0.0f, 0.0f, 1.0f));
				const dy::Math::float3 rawTangent = tangents[i];
				const dy::Math::float3 orthogonalTangent = Subtract(rawTangent, Scale(normal, Dot(normal, rawTangent)));
				const dy::Math::float3 tangent = NormalizeOr(orthogonalTangent, BuildFallbackTangent(normal));
				const float handedness = Dot(Cross(normal, tangent), bitangents[i]) < 0.0f ? -1.0f : 1.0f;
				vertex.normal = normal;
				vertex.tangent = dy::Math::float4(tangent.x, tangent.y, tangent.z, handedness);
			}
		}

		const char* GetTextureFilename(const ufbx_texture* texture)
		{
			if (!texture) return nullptr;
			if (texture->relative_filename.data && texture->relative_filename.data[0] != '\0')
			{
				return texture->relative_filename.data;
			}
			if (texture->filename.data && texture->filename.data[0] != '\0')
			{
				return texture->filename.data;
			}
			return nullptr;
		}

		void AssignTexturePath(const std::string& filepath, const char* textureName, std::string& outTexturePath)
		{
			if (!textureName || textureName[0] == '\0' || !outTexturePath.empty()) return;

			std::filesystem::path texturePath(textureName);
			if (texturePath.is_relative())
			{
				texturePath = std::filesystem::path(filepath).parent_path() / texturePath;
			}
			outTexturePath = texturePath.lexically_normal().string();
		}

		void AssignTextureMap(const std::string& filepath, const ufbx_material_map& map, std::string& outTexturePath)
		{
			if (!map.texture) return;
			AssignTexturePath(filepath, GetTextureFilename(map.texture), outTexturePath);
		}

		void AssignMetallicRoughnessTexture(const std::string& filepath, ufbx_material* material, MaterialTexturePaths& outTexturePaths)
		{
			const ufbx_texture* roughnessTexture = material->pbr.roughness.texture;
			const ufbx_texture* metalnessTexture = material->pbr.metalness.texture;
			if (roughnessTexture && roughnessTexture == metalnessTexture)
			{
				AssignTexturePath(filepath, GetTextureFilename(roughnessTexture), outTexturePaths.metallicRoughnessTexture);
			}
		}

		void AssignBaseColorValue(const ufbx_material_map& map, MaterialImportData& outMaterialData)
		{
			if (!map.has_value || outMaterialData.hasBaseColor || map.value_components < 3) return;
			const ufbx_vec4 value = map.value_vec4;
			const float alpha = map.value_components >= 4
				? static_cast<float>(value.w)
				: outMaterialData.material.baseColor.w;
			outMaterialData.material.baseColor = dy::Math::float4(
				static_cast<float>(value.x),
				static_cast<float>(value.y),
				static_cast<float>(value.z),
				std::clamp(alpha, 0.0f, 1.0f));
			outMaterialData.hasBaseColor = true;
		}

		void AssignEmissiveValue(const ufbx_material_map& map, MaterialImportData& outMaterialData)
		{
			if (!map.has_value || outMaterialData.hasEmissiveColor || map.value_components < 3) return;
			const ufbx_vec3 value = map.value_vec3;
			outMaterialData.material.emissiveColor = dy::Math::float3(
				static_cast<float>(value.x),
				static_cast<float>(value.y),
				static_cast<float>(value.z));
			outMaterialData.hasEmissiveColor = true;
		}

		void AssignScalarValue(const ufbx_material_map& map, float minValue, float maxValue, float& outValue, bool& outHasValue)
		{
			if (!map.has_value || outHasValue || map.value_components < 1) return;
			outValue = std::clamp(static_cast<float>(map.value_real), minValue, maxValue);
			outHasValue = true;
		}

		void ReadFirstMaterialData(const std::string& filepath, ufbx_scene* scene, MaterialImportData* outMaterialData)
		{
			if (!outMaterialData) return;
			*outMaterialData = {};

			for (size_t i = 0; i < scene->materials.count; ++i)
			{
				ufbx_material* material = scene->materials.data[i];
				AssignTextureMap(filepath, material->pbr.base_color, outMaterialData->texturePaths.baseColorTexture);
				AssignTextureMap(filepath, material->fbx.diffuse_color, outMaterialData->texturePaths.baseColorTexture);
				AssignMetallicRoughnessTexture(filepath, material, outMaterialData->texturePaths);
				AssignTextureMap(filepath, material->pbr.normal_map, outMaterialData->texturePaths.normalTexture);
				AssignTextureMap(filepath, material->fbx.normal_map, outMaterialData->texturePaths.normalTexture);
				AssignTextureMap(filepath, material->pbr.ambient_occlusion, outMaterialData->texturePaths.occlusionTexture);
				AssignTextureMap(filepath, material->pbr.emission_color, outMaterialData->texturePaths.emissiveTexture);
				AssignTextureMap(filepath, material->fbx.emission_color, outMaterialData->texturePaths.emissiveTexture);

				AssignBaseColorValue(material->pbr.base_color, *outMaterialData);
				AssignBaseColorValue(material->fbx.diffuse_color, *outMaterialData);
				AssignEmissiveValue(material->pbr.emission_color, *outMaterialData);
				AssignEmissiveValue(material->fbx.emission_color, *outMaterialData);
				AssignScalarValue(material->pbr.metalness, 0.0f, 1.0f, outMaterialData->material.metallicFactor, outMaterialData->hasMetallicFactor);
				AssignScalarValue(material->pbr.roughness, 0.04f, 1.0f, outMaterialData->material.roughnessFactor, outMaterialData->hasRoughnessFactor);
				AssignScalarValue(material->pbr.ambient_occlusion, 0.0f, 1.0f, outMaterialData->material.occlusionStrength, outMaterialData->hasOcclusionStrength);
				AssignScalarValue(material->fbx.bump_factor, 0.0f, 8.0f, outMaterialData->material.normalScale, outMaterialData->hasNormalScale);
				if (outMaterialData->HasAny()) return;
			}
		}
	}

	bool FBXLoader::Load(const std::string& filepath, MeshData& outData, std::string* outTexturePath)
	{
		MaterialTexturePaths texturePaths;
		const bool loaded = Load(filepath, outData, &texturePaths);
		if (outTexturePath)
		{
			*outTexturePath = texturePaths.baseColorTexture;
		}
		return loaded;
	}

	bool FBXLoader::Load(const std::string& filepath, MeshData& outData, MaterialTexturePaths* outTexturePaths)
	{
		MaterialImportData materialData;
		const bool loaded = Load(filepath, outData, &materialData);
		if (outTexturePaths)
		{
			*outTexturePaths = materialData.texturePaths;
		}
		return loaded;
	}

	bool FBXLoader::Load(const std::string& filepath, MeshData& outData, MaterialImportData* outMaterialData)
	{
		outData.vertices.clear();
		outData.indices.clear();
		if (outMaterialData) *outMaterialData = {};

		ufbx_load_opts opts = {};
		opts.load_external_files = true;

		ufbx_error error;
		ufbx_scene* scene = ufbx_load_file(filepath.c_str(), &opts, &error);
		if (!scene)
		{
			std::cerr << "Failed to load FBX file: " << filepath << "\nReason: " << error.info << std::endl;
			return false;
		}

		ReadFirstMaterialData(filepath, scene, outMaterialData);

		for (size_t nodeIndex = 0; nodeIndex < scene->nodes.count; ++nodeIndex)
		{
			ufbx_node* node = scene->nodes.data[nodeIndex];
			if (!node->mesh) continue;

			ufbx_mesh* mesh = node->mesh;
			std::vector<uint32_t> triangleIndices(mesh->max_face_triangles * 3);

			for (size_t faceIndex = 0; faceIndex < mesh->faces.count; ++faceIndex)
			{
				const ufbx_face face = mesh->faces.data[faceIndex];
				if (face.num_indices < 3) continue;

				const size_t triangleCount = ufbx_triangulate_face(triangleIndices.data(), triangleIndices.size(), mesh, face);
				for (size_t triangleIndex = 0; triangleIndex < triangleCount * 3; ++triangleIndex)
				{
					const uint32_t index = triangleIndices[triangleIndex];

					Vertex vertex;
					const ufbx_vec3 localPosition = ufbx_get_vertex_vec3(&mesh->vertex_position, index);
					const ufbx_vec3 worldPosition = ufbx_transform_position(&node->geometry_to_world, localPosition);
					vertex.position = dy::Math::float3(
						static_cast<float>(worldPosition.x),
						static_cast<float>(worldPosition.y),
						static_cast<float>(worldPosition.z));

					if (mesh->vertex_normal.exists)
					{
						const ufbx_vec3 localNormal = ufbx_get_vertex_vec3(&mesh->vertex_normal, index);
						const ufbx_vec3 worldNormal = ufbx_transform_direction(&node->geometry_to_world, localNormal);
						vertex.normal = NormalizeOr(
							dy::Math::float3(
								static_cast<float>(worldNormal.x),
								static_cast<float>(worldNormal.y),
								static_cast<float>(worldNormal.z)),
							dy::Math::float3(0.0f, 0.0f, 1.0f));
					}
					else
					{
						vertex.normal = dy::Math::float3(0.0f, 0.0f, 0.0f);
					}

					if (mesh->vertex_uv.exists)
					{
						const ufbx_vec2 uv = ufbx_get_vertex_vec2(&mesh->vertex_uv, index);
						vertex.uv = dy::Math::float2(static_cast<float>(uv.x), static_cast<float>(uv.y));
					}
					else
					{
						vertex.uv = dy::Math::float2(0.0f, 0.0f);
					}

					outData.indices.push_back(static_cast<uint32_t>(outData.vertices.size()));
					outData.vertices.push_back(vertex);
				}
			}
		}

		ufbx_free_scene(scene);

		FillMissingNormals(outData);
		CalculateTangents(outData);
		return !outData.vertices.empty() && !outData.indices.empty();
	}
}
