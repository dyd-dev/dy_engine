#ifndef MODEL_SHADER_LAYOUT_INC
#define MODEL_SHADER_LAYOUT_INC

// Renderer의 공개 셰이더 교체 경계에 맞춘 Model 전용 입력이다.
// 기본 draw 상수 192바이트 뒤에 offset 두 uint와 정렬 패딩 두 uint를 추가한다.
// 스키닝 버퍼와 추가 상수 16바이트는 Model 확장에서 바인딩한다.
#define RENDERER_DESCRIPTOR_SET 0
#define RENDERER_BINDING_SHADOW_MATRIX 3
#define RENDERER_BINDING_INLINE_CONSTANTS 10
#define RENDERER_BINDING_SKIN_INFLUENCES 11
#define RENDERER_BINDING_SKIN_PALETTE 12

#endif
