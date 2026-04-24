#version 450

layout(location = 0) in vec2 fragUV;
layout(location = 1) in vec3 fragNormal;

layout(location = 0) out vec4 outColor;

void main() {
    // Simple directional lighting
    vec3 lightDir = normalize(vec3(1.0, 1.0, -1.0));
    float diff = max(dot(normalize(fragNormal), lightDir), 0.0);
    
    vec3 ambient = vec3(0.1);
    vec3 diffuse = diff * vec3(0.8, 0.8, 0.8);
    
    // UV coloring for debugging
    vec3 albedo = vec3(fragUV, 0.5);
    
    outColor = vec4(albedo * (ambient + diffuse), 1.0);
}
