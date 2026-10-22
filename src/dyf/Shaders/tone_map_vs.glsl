#version 450
// 각 픽셀 위치에 맞게 정점 사이에서 섞일 UV를 출력한다. tone_map_ps는 이 좌표로 HDR 이미지를 읽는다.
layout(location=0) out vec2 uv;

// 정점 버퍼 없이 정점 번호 0·1·2로 전체 화면 삼각형과 UV를 만들어 톤 매핑할 HDR 이미지의 같은 위치를 읽게 한다.
void main() {
    // 정점 번호의 비트로 UV (0,0), (2,0), (0,2)를 만들어 화면을 덮는 큰 삼각형을 구성한다.
    uv=vec2((gl_VertexIndex<<1)&2,gl_VertexIndex&2);

    // UV에 (2,-2)를 곱하고 (-1,1)을 더해 투영 후 좌표를 만든다. 삼각형은 화면 밖까지 뻗지만 화면 안에서는 UV가 0~1을 덮어 HDR 이미지 전체를 읽는다.
    gl_Position=vec4(uv*vec2(2,-2)+vec2(-1,1),0,1);

}
