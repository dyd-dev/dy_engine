#version 450

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUv;

layout(location = 0) out vec3 fragNormal;
layout(location = 1) out vec2 fragUv;
layout(location = 2) out vec3 fragWorld;

layout(push_constant) uniform DrawConstants {
	mat4 worldViewProjection;
	mat4 world;
} pushConstants;

void main()
{
	vec4 worldPosition = pushConstants.world * vec4(inPosition, 1.0);
	gl_Position = pushConstants.worldViewProjection * vec4(inPosition, 1.0);
	fragWorld = worldPosition.xyz;
	fragNormal = normalize(mat3(pushConstants.world) * inNormal);
	fragUv = inUv;
}
