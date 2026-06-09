#version 450

layout(location = 0) in vec3 fragNormal;
layout(location = 1) in vec2 fragUv;
layout(location = 2) in vec3 fragWorld;

layout(location = 0) out vec4 outColor;

void main()
{
	vec3 normal = normalize(fragNormal);
	vec3 lightDir = normalize(vec3(-0.35, -0.8, -0.45));
	vec3 viewDir = normalize(vec3(0.0, 1.8, 4.5) - fragWorld);
	vec3 halfDir = normalize(-lightDir + viewDir);

	float nDotL = max(dot(normal, -lightDir), 0.0);
	float nDotH = max(dot(normal, halfDir), 0.0);
	float roughness = 0.48;
	float metallic = 0.0;
	float specPower = mix(96.0, 16.0, roughness);
	float specular = pow(nDotH, specPower) * mix(0.04, 0.22, metallic);

	vec3 baseColor = mix(vec3(0.18, 0.28, 0.42), vec3(0.74, 0.65, 0.48), fragUv.y);
	vec3 ambient = baseColor * 0.18;
	vec3 lit = ambient + baseColor * nDotL + vec3(specular);
	outColor = vec4(lit, 1.0);
}
