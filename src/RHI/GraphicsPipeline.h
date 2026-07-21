#pragma once
#include <cstdint>
#include "Format.h"
#include "IShader.h"

namespace dy::RHI
{
	enum class PrimitiveTopology : uint32_t
	{
		PointList,
		LineList,
		LineStrip,
		TriangleList,
		TriangleStrip
	};

	enum class VertexInputRate : uint32_t
	{
		PerVertex,
		PerInstance
	};

	struct VertexBindingDesc
	{
		uint32_t slot = 0;
		uint32_t stride = 0;
		VertexInputRate inputRate = VertexInputRate::PerVertex;
	};

	struct VertexAttributeDesc
	{
		// semanticName/semanticIndex는 DXIL 입력을, location은 SPIR-V/MSL 입력을 식별한다.
		const char* semanticName = nullptr;
		uint32_t semanticIndex = 0;
		uint32_t location = 0;
		uint32_t binding = 0;
		Format format = Format::Unknown;
		uint32_t offset = 0;
	};

	struct InputAssemblyDesc
	{
		PrimitiveTopology topology = PrimitiveTopology::TriangleList;
		const VertexBindingDesc* vertexBindings = nullptr;
		uint32_t vertexBindingCount = 0;
		const VertexAttributeDesc* vertexAttributes = nullptr;
		uint32_t vertexAttributeCount = 0;
	};

	struct GraphicsPipelineDesc
	{
		// 참조한 shader는 이 pipeline보다 오래 살아야 한다.
		IShader* vertexShader = nullptr;
		IShader* fragmentShader = nullptr;
		// 배열은 CreateGraphicsPipeline() 호출 동안만 유효해도 된다.
		InputAssemblyDesc inputAssembly = {};

		Format renderTargetFormat = Format::Unknown;
		Format depthStencilFormat = Format::Unknown;

		bool depthEnable = false;
		bool wireframe = false;
		bool enableBindlessTextures = false;

		int32_t depthBias = 0;
		float depthBiasSlope = 0.0f;
		float depthBiasClamp = 0.0f;
	};

	class IPipelineState
	{
	public:
		virtual ~IPipelineState() = default;
	};
}
