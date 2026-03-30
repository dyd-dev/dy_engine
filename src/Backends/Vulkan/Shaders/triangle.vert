#version 450

layout(location = 0) out vec2 fragTexCoord;

// 사각형을 만들기 위한 6개의 정점 (삼각형 2개)
vec2 positions[6] = vec2[](
    // 첫 번째 삼각형
    vec2(-0.5, 0.0), // 왼쪽 위
    vec2( 0.0, 0.5), // 오른쪽 위
    vec2( 0.0,  0.0), // 오른쪽 아래

    // 두 번째 삼각형
    vec2(-0.5, 0.0), // 왼쪽 위
    vec2( 0.0,  0.5), // 오른쪽 아래
    vec2( -0.5,  0.5)  // 왼쪽 아래
);

// 각 정점에 대응하는 텍스처 좌표 
// 각 정점에대해 절반 절반씩 구현을 해야한다 한가지 사각형이아닌 두가지 각형을 이어붙혀 한개의 텍스쳐로 보이게한다
vec2 texCoords[6] = vec2[](
     // 첫 번째 삼각형
    vec2(-1.0, 0.0), // 왼쪽 위
    vec2( 0.0, 1.0), // 오른쪽 위
    vec2( 0.0,  0.0), // 오른쪽 아래

    // 두 번째 삼각형
    vec2(-1.0, 0.0), // 왼쪽 위
    vec2( 0.0,  1.0), // 오른쪽 아래
    vec2( -1.0,  1.0)  // 왼쪽 아래
);

void main()
{
    gl_Position = vec4(positions[gl_VertexIndex], 0.0, 1.0);
    fragTexCoord = texCoords[gl_VertexIndex];
}