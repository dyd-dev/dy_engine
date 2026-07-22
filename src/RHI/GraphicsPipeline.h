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

	enum class FrontFace : uint32_t
	{
		// Backend viewport 변환 전 RHI clip-space에서의 winding이다.
		CounterClockwise,
		Clockwise
	};

	enum class CompareOp : uint32_t
	{
		Never,
		Less,
		Equal,
		LessEqual,
		Greater,
		NotEqual,
		GreaterEqual,
		Always
	};

	enum class StencilOp : uint32_t
	{
		Keep,
		Zero,
		Replace,
		IncrementClamp,
		DecrementClamp,
		Invert,
		IncrementWrap,
		DecrementWrap
	};

	struct RasterizationDesc
	{
		FillMode fillMode = FillMode::Solid;
		CullMode cullMode = CullMode::None;
		FrontFace frontFace = FrontFace::CounterClockwise;
		int32_t depthBias = 0;
		float depthBiasSlope = 0.0f;
		float depthBiasClamp = 0.0f;
	};

	struct StencilFaceDesc
	{
		StencilOp failOp = StencilOp::Keep;
		StencilOp depthFailOp = StencilOp::Keep;
		StencilOp passOp = StencilOp::Keep;
		CompareOp compareOp = CompareOp::Always;
	};

	struct DepthStencilDesc
	{
		// depthWriteEnable은 depthTestEnable을, stencilTestEnable은 stencil format을 요구한다.
		bool depthTestEnable = false;
		bool depthWriteEnable = false;
		CompareOp depthCompareOp = CompareOp::Less;
		bool stencilTestEnable = false;
		uint8_t stencilReadMask = 0xff;
		uint8_t stencilWriteMask = 0xff;
		uint32_t stencilReference = 0;
		StencilFaceDesc frontFace = {};
		StencilFaceDesc backFace = {};
	};

	enum ColorWrite : uint8_t
	{
		ColorWriteNone = 0,
		ColorWriteRed = 1u << 0,
		ColorWriteGreen = 1u << 1,
		ColorWriteBlue = 1u << 2,
		ColorWriteAlpha = 1u << 3,
		ColorWriteAll = ColorWriteRed | ColorWriteGreen | ColorWriteBlue | ColorWriteAlpha
	};

	enum class BlendFactor : uint32_t
	{
		Zero,
		One,
		SourceColor,
		OneMinusSourceColor,
		SourceAlpha,
		OneMinusSourceAlpha,
		DestinationColor,
		OneMinusDestinationColor,
		DestinationAlpha,
		OneMinusDestinationAlpha
	};

	enum class BlendOp : uint32_t
	{
		Add,
		Subtract,
		ReverseSubtract,
		Min,
		Max
	};

	struct ColorAttachmentDesc
	{
		Format format = Format::Unknown;
		uint8_t writeMask = ColorWriteAll;
		bool blendEnable = false;
		BlendFactor sourceColorFactor = BlendFactor::One;
		BlendFactor destinationColorFactor = BlendFactor::Zero;
		BlendOp colorOp = BlendOp::Add;
		BlendFactor sourceAlphaFactor = BlendFactor::One;
		BlendFactor destinationAlphaFactor = BlendFactor::Zero;
		BlendOp alphaOp = BlendOp::Add;
	};

	struct GraphicsPipelineDesc
	{
		// 참조한 shader는 이 pipeline보다 오래 살아야 한다.
		IShader* vertexShader = nullptr;
		IShader* fragmentShader = nullptr;
		// 배열은 CreateGraphicsPipeline() 호출 동안만 유효해도 된다.
		InputAssemblyDesc inputAssembly = {};
		RasterizationDesc rasterization = {};
		DepthStencilDesc depthStencil = {};
		// 배열은 CreateGraphicsPipeline() 호출 동안만 유효해도 된다.
		const ColorAttachmentDesc* colorAttachments = nullptr;
		uint32_t colorAttachmentCount = 0;

		Format depthStencilFormat = Format::Unknown;

		bool enableBindlessTextures = false;
	};

	class IPipelineState
	{
	public:
		virtual ~IPipelineState() = default;
	};
}
