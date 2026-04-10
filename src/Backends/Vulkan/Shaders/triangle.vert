#version 450

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inTexCoord;

layout(location = 0) out vec3 fragNormal;

layout(push_constant) uniform PushConstants {
    float time;
} push;

void main() {
    // 1. 모델 크기를 줄임. (원래 4.5 ~ 8.0 크기이므로 0.05배 하면 최대 0.4 크기가 됨)
    vec3 p = inPosition * 0.05;
    vec3 n = inNormal;
    
    // 2. Y축 기준 45도 회전 (좌우 대각선)
    float rotY = 0.785398; // 45 degrees in radians
    float sy = sin(rotY), cy = cos(rotY);
    
    float px1 = p.x * cy - p.z * sy;
    float pz1 = p.x * sy + p.z * cy;
    p.x = px1; p.z = pz1;
    
    float nx1 = n.x * cy - n.z * sy;
    float nz1 = n.x * sy + n.z * cy;
    n.x = nx1; n.z = nz1;

    // 3. X축 기준 35.264도 회전 (앞으로 숙이기)
    float rotX = 0.615472; // ~35.264 degrees in radians
    float sx = sin(rotX), cx = cos(rotX);
    
    float py2 = p.y * cx - p.z * sx;
    float pz2 = p.y * sx + p.z * cx;
    p.y = py2; p.z = pz2;
    
    float ny2 = n.y * cx - n.z * sx;
    float nz2 = n.y * sx + n.z * cx;
    n.y = ny2; n.z = nz2;

    // 4. 화면에 출력 (Z값을 약간 뒤로 밀어서 클리핑 방지)
    gl_Position = vec4(p.x, p.y, p.z + 0.5, 1.0);
    
    fragNormal = n;
}
