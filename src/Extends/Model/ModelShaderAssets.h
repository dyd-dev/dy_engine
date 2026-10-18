#pragma once

#include "dyf/RHI/Shader.h"

namespace dyf
{
    // 선택 Model 모듈의 내장 바이너리 뷰다. 수명은 프로그램 전체에 걸쳐 유지된다.
    RHI::ShaderDesc GetModelVertexShader(bool shadowsEnabled);
    RHI::ShaderDesc GetModelShadowShader();
    RHI::ShaderDesc GetModelComputeShader();
}
