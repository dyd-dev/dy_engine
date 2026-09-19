#include "dyf.h"

int main()
{
	const uint32_t width = 640, height = 480;

	dyf::Platform::Window window(width, height, "dyf / Geometry and Camera");
	if(!window.GetHandle()) return 1;

	auto renderer = dyf::Renderer::Create(window.GetHandle());
	if(!renderer) return 1;

	const dyf::Math::float3 positions[] = {
		{-0.9f, 0, -0.6f}, {0.9f, 0, -0.6f}, {0, 0, 0.9f}
	};
	dyf::MeshData triangle;
	for(const auto& position : positions)
	{
		dyf::Vertex vertex;
		vertex.position = position;
		vertex.normal = {0, -1, 0};
		vertex.uv = {0, 0};
		triangle.vertices.push_back(vertex);
	}
	triangle.indices = {0, 1, 2};

	dyf::MaterialDesc material;
	material.baseColor = {0.2f, 0.7f, 1, 1};

	dyf::Camera camera;
	dyf::Math::float3 eye = {0, -3, 1.6f}, target = {0, 0, 0};
	if(!camera.LookAt(eye, target)) return 1;
	const float aspect = width / static_cast<float>(height);
	if(!camera.SetPerspective(aspect)) return 1;

	while(true)
	{
		window.PollEvents();
		if(!window.IsRunning()) break;

		if(!renderer->Render(triangle, material, camera)) return 1;
	}
}
