#pragma once

#include "RHI/Shader.h"

namespace dy::Backends
{
    struct D3D12ObjectDeleter;

    class D3D12Shader final : public RHI::Shader
    {
    public:
        explicit D3D12Shader(const RHI::ShaderDesc& desc)
            : RHI::Shader(desc)
        {
        }

    private:
        friend struct D3D12ObjectDeleter;

        ~D3D12Shader() override = default;
    };
}
