#pragma once

#include <cstdint>
#include <vector>

#include "Binding.h"
#include "Format.h"
#include "ResourceHandles.h"

namespace dyf::RHI
{
	enum class PrimitiveTopology : uint8_t
	{
		Undefined,
		PointList,
		LineList,
		TriangleList,
		TriangleStrip
	};

	enum class VertexStepMode : uint8_t
	{
		Undefined,
		Vertex,
		Instance
	};

	struct VertexBufferLayout
	{
		uint32_t binding = 0;
		uint32_t stride = 0;
		VertexStepMode stepMode = VertexStepMode::Undefined;
	};

	struct VertexAttribute
	{
		uint32_t location = 0;
		uint32_t binding = 0;
		Format format = Format::Unknown;
		uint32_t offset = 0;
	};

	enum class FillMode : uint8_t
	{
		Undefined,
		Solid,
		Wireframe
	};

	enum class CullMode : uint8_t
	{
		Undefined,
		None,
		Front,
		Back
	};

	enum class FrontFace : uint8_t
	{
		Undefined,
		CounterClockwise,
		Clockwise
	};

	enum class CompareOp : uint8_t
	{
		Undefined,
		Never,
		Less,
		Equal,
		LessEqual,
		Greater,
		NotEqual,
		GreaterEqual,
		Always
	};

	enum class StencilOp : uint8_t
	{
		Undefined,
		Keep,
		Zero,
		Replace,
		IncrementClamp,
		DecrementClamp,
		Invert,
		IncrementWrap,
		DecrementWrap
	};

	struct StencilFaceState
	{
		StencilOp failOp = StencilOp::Undefined;
		StencilOp depthFailOp = StencilOp::Undefined;
		StencilOp passOp = StencilOp::Undefined;
		CompareOp compareOp = CompareOp::Undefined;
	};

	struct RasterState
	{
		FillMode fillMode = FillMode::Undefined;
		CullMode cullMode = CullMode::Undefined;
		FrontFace frontFace = FrontFace::Undefined;
        // 깊이 포맷의 네이티브 바이어스 단위에 곱할 상수 계수다. 소수 지원은 FractionalDepthBias로 조회한다.
		float depthBiasConstant = 0.0f;
		float depthBiasSlope = 0.0f;
		float depthBiasClamp = 0.0f;
	};

	struct DepthStencilState
	{
		// 비활성 기능의 세부 필드는 사용하지 않는다. depthTestEnabled=false이면
		// depthCompareOp는 사용하지 않으며, 깊이 쓰기만 켠 경우 항상 통과하는 비교로 구현한다.
		Format format = Format::Unknown;
		bool depthTestEnabled = false;
		bool depthWriteEnabled = false;
		CompareOp depthCompareOp = CompareOp::Undefined;
		bool stencilEnabled = false;
		uint8_t stencilReadMask = 0;
		uint8_t stencilWriteMask = 0;
		StencilFaceState front = {};
		StencilFaceState back = {};
	};

	enum class BlendFactor : uint8_t
	{
		Undefined,
		Zero,
		One,
		SourceColor,
		OneMinusSourceColor,
		DestinationColor,
		OneMinusDestinationColor,
		SourceAlpha,
		OneMinusSourceAlpha,
		DestinationAlpha,
		OneMinusDestinationAlpha
	};

	enum class BlendOp : uint8_t
	{
		Undefined,
		Add,
		Subtract,
		ReverseSubtract,
		Min,
		Max
	};

	enum class ColorWriteMask : uint8_t
	{
		None = 0,
		Red = 1u << 0u,
		Green = 1u << 1u,
		Blue = 1u << 2u,
		Alpha = 1u << 3u,
		All = (1u << 0u) | (1u << 1u) | (1u << 2u) | (1u << 3u)
	};

	inline constexpr ColorWriteMask operator|(ColorWriteMask left, ColorWriteMask right)
	{
		return static_cast<ColorWriteMask>(static_cast<uint8_t>(left) | static_cast<uint8_t>(right));
	}

	struct BlendState
	{
		// enabled=false이면 계수/연산은 사용하지 않는다. Min/Max 연산도 계수를 사용하지 않는다.
		// enabled=true이면 Min/Max를 포함하여 모든 계수에 유효한 enum(예: One/Zero)을 지정한다.
		// 알파 항의 Color 계수는 해당 색의 알파 성분이다. D3D12는 동등한 Alpha 계수로
		// 변환하고 파이프라인 생성 시 Info 진단을 전달한다.
		bool enabled = false;
		BlendFactor sourceColor = BlendFactor::Undefined;
		BlendFactor destinationColor = BlendFactor::Undefined;
		BlendOp colorOp = BlendOp::Undefined;
		BlendFactor sourceAlpha = BlendFactor::Undefined;
		BlendFactor destinationAlpha = BlendFactor::Undefined;
		BlendOp alphaOp = BlendOp::Undefined;
	};

	struct ColorAttachmentDesc
	{
		Format format = Format::Unknown;
		BlendState blend = {};
		ColorWriteMask writeMask = ColorWriteMask::None;
	};

	struct PipelineLayoutDesc
	{
		const ResourceBindingLayout* bindings = nullptr;
		uint32_t bindingCount = 0;
		uint32_t inlineConstantSize = 0;
		ShaderStageFlags inlineConstantStages = ShaderStageFlags::None;
		uint32_t inlineConstantBinding = 0;
	};

	struct GraphicsPipelineDesc
	{
		ShaderHandle vertexShader = nullptr;
		ShaderHandle fragmentShader = nullptr;
		PrimitiveTopology topology = PrimitiveTopology::Undefined;
		const VertexBufferLayout* vertexBuffers = nullptr;
		uint32_t vertexBufferCount = 0;
		const VertexAttribute* vertexAttributes = nullptr;
		uint32_t vertexAttributeCount = 0;
		RasterState raster = {};
		DepthStencilState depthStencil = {};
		const ColorAttachmentDesc* colorAttachments = nullptr;
		uint32_t colorAttachmentCount = 0;
		PipelineLayoutDesc layout = {};
	};

    struct ComputePipelineDesc
    {
        ShaderHandle computeShader=nullptr;
        PipelineLayoutDesc layout={};
    };

	class Pipeline
	{
	public:
		[[nodiscard]] bool IsCompute() const {return m_compute;}
        [[nodiscard]] const PipelineLayoutDesc& GetLayout() const { return m_layout; }

	protected:
		virtual ~Pipeline() = default;
		explicit Pipeline(const PipelineLayoutDesc& layout,bool compute=false)
			: m_layout(layout),m_compute(compute)
		{
			if(layout.bindings != nullptr && layout.bindingCount != 0)
			{
				m_bindings.assign(layout.bindings, layout.bindings + layout.bindingCount);
				m_layout.bindings = m_bindings.data();
			}
			else
			{
				m_layout.bindings = nullptr;
				m_layout.bindingCount = 0;
			}
		}

	private:
		friend class IDevice;
		friend class ICommandList;
		// 공통 draw 검증용 생성 시점의 선언. 호출자 descriptor 메모리를 참조하지 않는다.
		std::vector<uint32_t> m_requiredVertexBindings;
		std::vector<Format> m_colorFormats;
		Format m_depthStencilFormat = Format::Unknown;
		PipelineLayoutDesc m_layout = {};
        bool m_compute=false;
		std::vector<ResourceBindingLayout> m_bindings;
	};
}
