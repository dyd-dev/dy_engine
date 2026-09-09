	struct RendererDirectionalLight
	{
		DY_LIGHT_FLOAT4 directionIntensity;
		DY_LIGHT_FLOAT4 color;
	};

	struct RendererPointLight
	{
		DY_LIGHT_FLOAT4 positionRange;
		DY_LIGHT_FLOAT4 colorIntensity;
	};

	struct RendererSpotLight
	{
		DY_LIGHT_FLOAT4 positionRange;
		DY_LIGHT_FLOAT4 directionOuterCos;
		DY_LIGHT_FLOAT4 colorIntensity;
		DY_LIGHT_FLOAT4 coneParams;
	};

	struct RendererRectAreaLight
	{
		DY_LIGHT_FLOAT4 positionIntensity;
		DY_LIGHT_FLOAT4 directionWidth;
		DY_LIGHT_FLOAT4 upHeight;
		DY_LIGHT_FLOAT4 color;
	};

	struct RendererDiscAreaLight
	{
		DY_LIGHT_FLOAT4 positionIntensity;
		DY_LIGHT_FLOAT4 directionRadius;
		DY_LIGHT_FLOAT4 up;
		DY_LIGHT_FLOAT4 color;
	};

	struct RendererLightingConstants
	{
		DY_LIGHT_FLOAT4 cameraPosition;
		DY_LIGHT_FLOAT4 directionalLightDirection;
		DY_LIGHT_FLOAT4 directionalLightColor;
		DY_LIGHT_FLOAT4 ambientColor;
		DY_LIGHT_FLOAT4 shadowParams;
		DY_LIGHT_FLOAT4 pbrParams;
		DY_LIGHT_FLOAT4 environmentColor;
		DY_LIGHT_FLOAT4 pointLightPositionRange;
		DY_LIGHT_FLOAT4 pointLightColorIntensity;
		DY_LIGHT_FLOAT4 lightCounts;
		DY_LIGHT_FLOAT4 areaLightCounts;
		RendererDirectionalLight directionalLights[RENDERER_MAX_DIRECTIONAL_LIGHTS];
		RendererPointLight pointLights[RENDERER_MAX_POINT_LIGHTS];
		RendererSpotLight spotLights[RENDERER_MAX_SPOT_LIGHTS];
		RendererRectAreaLight rectAreaLights[RENDERER_MAX_RECT_AREA_LIGHTS];
		RendererDiscAreaLight discAreaLights[RENDERER_MAX_DISC_AREA_LIGHTS];
		DY_LIGHT_FLOAT4 shadowLight;
	};
