cbuffer Transform : register(b15) { float4 transform; float4 settings; };

// CPU가 만든 Canvas 정점의 픽셀 위치·이미지 UV·곱할 색을 받는 입력이다.
struct Input { float2 position:TEXCOORD0; float2 uv:TEXCOORD1; float4 color:TEXCOORD2; };

// 투영 후 잘라내기 전 좌표 clip과 UV·곱할 색을 반환한다. UV와 색은 삼각형 안의 픽셀 위치에 맞게 정점 사이의 값이 섞인다.
struct Output { float4 position:SV_POSITION; float2 uv:TEXCOORD0; float4 color:TEXCOORD1; };

// CPU가 보낸 배율·이동값으로 Canvas 픽셀 위치를 투영 후 화면 좌표에 놓고 UV·색은 그대로 전달한다.
Output main(Input input) { Output o; o.position=float4(input.position*transform.xy+transform.zw,0,1); o.uv=input.uv; o.color=input.color; return o; }
