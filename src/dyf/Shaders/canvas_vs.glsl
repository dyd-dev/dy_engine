#version 450
// Canvas 정점의 픽셀 위치·이미지 UV·곱할 색을 받는다. 위치만 화면 공간으로 변환한다.
layout(location=0) in vec2 position;
layout(location=1) in vec2 uv;
layout(location=2) in vec4 color;

// value의 배율·이동은 정점 위치 변환에, settings.x의 sRGB 여부는 픽셀 색 출력에 쓰이는 공통 상수다.
layout(push_constant) uniform Transform { vec4 value; vec4 settings; } transform;

// 이미지 읽기 위치 UV와 곱할 색을 그대로 넘겨 삼각형 안의 각 픽셀에서 정점 사이 값을 보간하게 한다.
layout(location=0) out vec2 outUv;
layout(location=1) out vec4 outColor;

// CPU가 보낸 배율·이동값으로 Canvas 픽셀 위치를 투영 후 화면 좌표에 놓는다.
void main() { gl_Position=vec4(position*transform.value.xy+transform.value.zw,0,1); outUv=uv; outColor=color; }
