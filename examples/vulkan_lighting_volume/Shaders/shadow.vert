#version 450

// Shadow Map용 Depth-Only Vertex Shader.
// 광원 시점에서 깊이만 기록하면 되므로 Fragment Shader는 필요 없음
// (Vulkan은 FS 없이도 depth attachment만 있으면 동작).
//
// PushConstants는 메인 패스와 같은 layout을 재사용해 PipelineLayout 호환성 유지.
// 단, viewProj는 무시하고 binding=3의 lightViewProj로 대체한다.

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;   // 미사용
layout(location = 2) in vec2 inUV;       // 미사용

layout(push_constant) uniform PushConstants {
    mat4 viewProj;  // 메인 패스용. 여기서는 사용 X
    mat4 model;
} pushConstants;

layout(set = 0, binding = 3) uniform ShadowMatrix {
    mat4 lightViewProj;
} shadowMatrix;

void main() {
    // 메인 셰이더와 동일한 drawMode 트릭 처리
    mat4 model = pushConstants.model;
    float drawMode = model[3][3];
    model[3][3] = 1.0;

    // 그림자(drawMode<0)와 바닥(drawMode>1.5)은 caster가 아니므로 컬링.
    // gl_Position의 w를 0으로 만들어 화면 밖으로 보냄.
    if (drawMode < 0.0 || drawMode > 1.5) {
        gl_Position = vec4(2.0, 2.0, 2.0, 1.0); // NDC 밖 → 클리핑됨
        return;
    }

    vec4 worldPosition = model * vec4(inPosition, 1.0);
    gl_Position = shadowMatrix.lightViewProj * worldPosition;
}
