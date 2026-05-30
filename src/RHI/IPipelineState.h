#pragma once
#include <cstddef>
#include <cstdint>
#include "Enums.h"
/* PipelineState.h
* 
* PipelineState는 그래픽스 파이프라인의 상태를 정의하는 클래스입니다.
* Shader, BlendState, RasterizerState, DepthStencilState 등과 같은 그래픽스 파이프라인의 다양한 상태를 캡슐화하여 관리합니다.
*/

namespace dy::RHI
{
	enum class PrimitiveTopology : uint32_t
	{
		TriangleList,
		LineList
	};

	enum class FillMode : uint32_t
	{
		Solid,
		Wireframe
	};

	enum class CullMode : uint32_t
	{
		None,
		Front,
		Back
	};

	enum class CompareOp : uint32_t
	{
		Never,
		Less,
		LessEqual,
		Equal,
		GreaterEqual,
		Greater,
		Always
	};

	struct RasterizerStateDesc
	{
		FillMode fillMode = FillMode::Solid;
		CullMode cullMode = CullMode::Back;
		bool frontCounterClockwise = true;
	};

	struct DepthStencilStateDesc
	{
		bool depthTestEnable = false;
		bool depthWriteEnable = false;
		CompareOp depthCompare = CompareOp::LessEqual;
	};

	struct BlendStateDesc
	{
		bool alphaBlendEnable = true;
	};

	// Shadow pass configuration. Kept separate so non-shadow pipelines pay no overhead.
	struct ShadowPipelineDesc
	{
		const void* shadowVertexShader = nullptr;
		size_t shadowVertexShaderSize = 0;
		uint32_t shadowMapResolution = 2048;
		bool enableShadowPass = false;
	};

	struct GraphicsPipelineDesc {
		const void* vertexShader = nullptr;
		size_t vertexShaderSize = 0;

		const void* pixelShader = nullptr;
		size_t pixelShaderSize = 0;

		Format renderTargetFormat = Format::Unknown;
		Format depthStencilFormat = Format::Unknown;

		PrimitiveTopology topology = PrimitiveTopology::TriangleList;
		RasterizerStateDesc rasterizer = {};
		DepthStencilStateDesc depthStencil = {};
		BlendStateDesc blend = {};
		ShadowPipelineDesc shadow = {};

		// Legacy convenience fields. Backends may keep using these while they migrate
		// to the structured state above.
		bool depthEnable = false;
		bool wireframe = false;
	};

	struct ComputePipelineDesc {
		const void* computeShader = nullptr;
		size_t computeShaderSize = 0;
	};

	class IPipelineState
	{
	public:
		virtual ~IPipelineState() = default;
	};
}
