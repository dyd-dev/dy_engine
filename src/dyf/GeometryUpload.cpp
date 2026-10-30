#include "RendererInternal.h"
#include "DrawCommands.h"
#include "dyf/Scene.h"
#include "dyf/RHI/Buffer.h"
#include "dyf/RHI/IDevice.h"
#include <cmath>
#include <utility>
#include <vector>

namespace dyf
{
namespace
{
	// 소유한 버퍼를 장치로 반환하고 핸들/업로드 상태를 함께 초기화한다. 이후 코드가 과거 핸들을 준비된 자원으로 재사용하지 않게 한다.
	void DestroyBuffer(RHI::IDevice* device, RHI::BufferHandle& buffer, bool& ready)
	{
		if(device != nullptr && buffer != nullptr) device->DestroyBuffer(buffer); // 장치와 버퍼가 있을 때만 해제하고 이후 핸들은 항상 비운다.
		buffer = nullptr;
		ready = false;
	}

	// 요청 크기와 같으면 기존 버퍼를 유지한다. 크기가 다르면 기존 버퍼를 해제하고 CopyDestination 버퍼를 만든다. 생성 실패 시 buffer는 nullptr, ready는 false로 남으며 이전 버퍼를 복구하지 않는다.
	bool EnsureBuffer(RHI::IDevice* device, RHI::BufferHandle& buffer, bool& ready,
		uint32_t sizeBytes, uint32_t stride, RHI::BufferUsage usage)
	{
		if(device == nullptr) return false; // 버퍼 생성에 필요한 장치가 없으면 실패한다.
		if(buffer != nullptr && buffer->GetDesc().size == sizeBytes) return true; // 동일 크기의 기존 버퍼는 재생성하지 않는다.
		DestroyBuffer(device, buffer, ready);
		buffer = device->CreateBuffer({sizeBytes, stride, usage, RHI::ResourceState::CopyDestination});
		return buffer != nullptr;
	}
}

// 한 메시의 원본/LOD 버퍼, 인덱스 수, 경계, 준비 상태를 함께 정리한다. 이 함수 자체는 Scene의 CPU MeshData를 삭제하지 않는다.
void Renderer::Impl::DestroyMeshState(RHI::IDevice* device, SceneMeshState& mesh)
{
	DestroyBuffer(device, mesh.vertexBuffer, mesh.vertexReady);
	DestroyBuffer(device, mesh.indexBuffer, mesh.indexReady);
	for(auto& lod:mesh.lods) DestroyBuffer(device,lod.buffer,lod.ready);
	mesh.lods.clear();mesh.lodsGenerated=false;
	mesh.indexCount = 0;
    mesh.bounds={};
	mesh.prepared = false;
}

// Scene 크기에 캐시를 맞춰 변경 메시지만 준비하며, UpdateBuffer가 바이트를 복사하므로 임시 CPU 변환 배열은 기록 직후 해제할 수 있다.
bool Renderer::Impl::PrepareGeometry(const Scene& scene, RHI::IDevice* device)
{
	if(device == nullptr) return false; // 지오메트리를 준비할 장치가 없으면 중단한다.
	while(m_meshes.size() > scene.Meshes().size()) // Scene에서 사라진 뒤쪽 메시 캐시를 하나씩 정리한다.
	{
		DestroyMeshState(device, m_meshes.back());
		m_meshes.pop_back();
	}
	m_meshes.resize(scene.Meshes().size());

	RHI::ICommandList* commands = nullptr;
	// 획득한 명령 목록의 소유권을 범위로 묶어 조기 반환에서도 장치에 돌려준다.
	RHI::ResourceScope resources(*device);
	bool uploadFailed = false;
	std::vector<bool*> uploaded;
	for(uint32_t meshIndex = 0; meshIndex < scene.Meshes().size(); ++meshIndex)
	{
		SceneMeshState& mesh = m_meshes[meshIndex];
		const auto& sourceMesh = scene.Meshes()[meshIndex];
		if(mesh.source.lock() != sourceMesh) // 원본 MeshData가 달라졌거나 이전 원본이 소멸했으면 GPU 캐시를 폐기한다.
		{
			DestroyMeshState(device, mesh);
			mesh.source = sourceMesh;
		}
		if(mesh.prepared && (!config.enableLod || mesh.lodsGenerated)) continue; // 업로드 제출을 마쳤거나 빈 메시로 확정된 상태에서, LOD가 꺼져 있거나 LOD 생성 결과가 있으면 재사용한다. prepared는 GPU 완료 확인값이 아니다.
        const MeshData& source = *sourceMesh;
		mesh.prepared=false;
        mesh.bounds={};
        // 유한한 위치만 경계에 포함한다. 잘못된 위치가 발견되면 경계를 무효화하여 잘못된 경계 기반 제거에 의존하지 않게 한다.
        for(const auto& vertex:source.vertices) {
            const auto& p=vertex.position;
            if(!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z)) {mesh.bounds={};break;} // 비유한 위치 하나라도 있으면 경계를 무효화하고 경계 계산을 멈춘다.
            mesh.bounds.Include(p);
        }
		if(source.vertices.size() > UINT32_MAX / sizeof(RendererVertex) || // 정점/인덱스 바이트 수가 32비트 크기를 넘으면 실패한다.
			source.indices.size() > UINT32_MAX / sizeof(uint32_t))
		{
			return false;
		}
		// 공개 Mesh 정점의 표면에 수직인 방향(법선)과 표면을 따라가는 방향(접선)을 렌더러 입력에 복사한다.
		// 위치·법선·UV·접선과 방향 부호를 셰이더 입력에 맞는 48바이트 RendererVertex로 변환한다.
		std::vector<RendererVertex> vertices;
		vertices.reserve(source.vertices.size());
		for(const auto& vertex : source.vertices)
		{
			vertices.push_back({vertex.position.x, vertex.position.y, vertex.position.z,
				vertex.normal.x, vertex.normal.y, vertex.normal.z, vertex.uv.x, vertex.uv.y,
				vertex.tangent.x, vertex.tangent.y, vertex.tangent.z, vertex.tangent.w});
		}
		std::vector<uint32_t> sequentialIndices;
		if(source.indices.empty()) // 인덱스를 생략한 메시에는 0부터 연속 인덱스를 만들어 같은 indexed draw 경로를 사용한다.
		{
			sequentialIndices.reserve(source.vertices.size());
			for(uint32_t index = 0; index < source.vertices.size(); ++index) sequentialIndices.push_back(index);
		}
		const auto& indices = source.indices.empty() ? sequentialIndices : source.indices;
		if(vertices.empty()) // 빈 메시는 GPU 자원을 비우되 준비 처리된 빈 결과로 저장한다.
		{
			DestroyMeshState(device, mesh);
			mesh.prepared = true;
			continue;
		}
		if(config.enableLod && !mesh.lodsGenerated) // LOD가 켜져 있고 아직 생성하지 않은 메시만 LOD를 준비한다.
		{
			if(source.lods.size()>3) return false; // 입력 LOD가 최대 세 단계를 넘으면 거부한다.
			// 명시 LOD가 있으면 그 입력을 사용하고 없으면 CPU 단순화 결과를 구한다. 원본 이후 최대 세 단계이며 인덱스 감소와 오차의 비감소를 검사한다.
			auto levels=source.lods.empty() ? BuildMeshLods(source) : source.lods;
			if(levels.size()>3) return false; // 자동 생성 결과도 같은 최대 단계 수를 지켜야 한다.
			uint32_t previousCount=static_cast<uint32_t>(indices.size());float previousError=0;
			for(const auto& level:levels)
			{
				if(level.indices.empty() || level.indices.size()%3 || level.indices.size()>=previousCount || // 인덱스가 없거나 3의 배수가 아니거나 이전 단계보다 줄지 않으면 거부한다. 32비트 바이트 범위를 넘거나 오차가 비유한 값 또는 이전 단계보다 작아도 거부한다.
					level.indices.size()>UINT32_MAX/sizeof(uint32_t) || !std::isfinite(level.error) || level.error<previousError)
					return false;
				for(uint32_t index:level.indices) if(index>=vertices.size()) return false; // LOD 인덱스가 실제 정점 범위를 벗어나면 업로드하지 않는다.
				previousCount=static_cast<uint32_t>(level.indices.size());previousError=level.error;
			}
			// 검사가 끝난 LOD를 먼저 임시 배열에 모은 뒤 mesh에 옮긴다. 각 단계의 CPU 인덱스는 업로드 제출 성공 후 비운다.
			std::vector<SceneMeshState::IndexLod> pendingLods;
			pendingLods.reserve(levels.size());
			for(auto& level:levels)
			{
				SceneMeshState::IndexLod lod;lod.count=static_cast<uint32_t>(level.indices.size());lod.error=level.error;
				lod.indices=std::move(level.indices);pendingLods.push_back(std::move(lod));
			}
			mesh.lods=std::move(pendingLods);
			// 생성된 LOD가 없어도 lodsGenerated를 켜서, 더 줄일 수 없는 메시를 다음 호출에서 다시 단순화하지 않는다.
			mesh.lodsGenerated=true;
		}
		const uint32_t vertexBytes = static_cast<uint32_t>(vertices.size() * sizeof(RendererVertex));
		const uint32_t indexBytes = static_cast<uint32_t>(indices.size() * sizeof(uint32_t));
		if(!EnsureBuffer(device, mesh.vertexBuffer, mesh.vertexReady, vertexBytes, // 정점 또는 원본 인덱스 버퍼를 확보하지 못하면 준비를 중단한다.
				sizeof(RendererVertex), RHI::BufferUsage::Vertex | RHI::BufferUsage::Storage) ||
			!EnsureBuffer(device, mesh.indexBuffer, mesh.indexReady, indexBytes,
				sizeof(uint32_t), RHI::BufferUsage::Index))
		{
			return false;
		}
		mesh.indexCount = static_cast<uint32_t>(indices.size());
		if(!commands) // 업로드할 메시를 처음 만났을 때만 공통 명령 목록을 획득한다.
		{
			commands = device->AcquireCommandList();
			if(commands) resources.Keep(commands); // 획득에 성공한 명령 목록만 범위 소유자에 등록한다.
		}
		if(!commands) return false; // 명령 목록을 얻지 못했으면 데이터를 기록할 수 없다.
		if(!mesh.vertexReady) // 아직 제출되지 않은 정점 버퍼만 업로드를 기록한다.
		{
			if(!RecordBufferUpload(device, *commands, mesh.vertexBuffer, vertices.data(), vertexBytes, // 정점 업로드 기록 실패를 표시하고 메시 준비 반복을 중단한다.
				RHI::ResourceState::VertexBuffer))
			{
				uploadFailed = true;
				break;
			}
			uploaded.push_back(&mesh.vertexReady);
		}
		if(!mesh.indexReady) // 아직 제출되지 않은 원본 인덱스 버퍼만 기록한다.
		{
			if(!RecordBufferUpload(device, *commands, mesh.indexBuffer, indices.data(), indexBytes, // 인덱스 업로드 기록 실패면 전체 준비 결과를 실패로 남긴다.
				RHI::ResourceState::IndexBuffer))
			{
				uploadFailed = true;
				break;
			}
			uploaded.push_back(&mesh.indexReady);
		}
		for(auto& lod:mesh.lods)
		{
			if(lod.ready) continue; // 이미 제출한 LOD 버퍼는 다시 보내지 않는다.
			const uint32_t bytes=lod.count*sizeof(uint32_t);
			if(lod.indices.size()!=lod.count || !EnsureBuffer(device,lod.buffer,lod.ready,bytes, // LOD CPU 개수·GPU 버퍼 확보·업로드 기록 중 하나라도 실패하면 중단한다.
				sizeof(uint32_t),RHI::BufferUsage::Index) ||
				!RecordBufferUpload(device,*commands,lod.buffer,lod.indices.data(),bytes,RHI::ResourceState::IndexBuffer))
			{uploadFailed=true;break;}
			uploaded.push_back(&lod.ready);
		}
		if(uploadFailed) break; // LOD 준비에서 생긴 실패도 바깥 메시 반복까지 중단시킨다.
	}

	if(!commands) return true; // 모든 메시가 재사용/빈 상태여서 기록할 명령이 없으면 제출 없이 성공한다.
	// 여기까지 도달하면 명령 목록을 닫고 같은 큐에 제출한다. uploadFailed가 켜진 기록 실패 경로도 이미 기록된 명령은 제출할 수 있으며, 제출 성공과 전체 함수 성공은 별도로 판정한다.
	const bool closed = commands->Close();
	const bool submitted = closed && device->Submit(&commands, 1);
	// 이번 제출이 성공한 버퍼만 ready로 확정하며 GPU 완료를 뜻하지 않는다. 제출 실패 시에는 ready를 새로 확정하지 않아 다음 프레임에 재업로드한다.
	if(submitted)
	{
		// 일부 메시 기록에 실패했다면 제출한 버퍼를 확정해도 전체 함수는 false를 반환한다.
		for(bool* ready : uploaded) *ready = true;
		for(auto& mesh : m_meshes)
		{
			bool lodsReady=true;
			for(auto& lod:mesh.lods)
			{
				lodsReady=lodsReady && lod.ready;
				if(lod.ready) std::vector<uint32_t>().swap(lod.indices); // LOD 업로드가 제출되었으면 재시도용 CPU 인덱스 저장소를 비운다.
			}
			if(mesh.vertexReady && mesh.indexReady && lodsReady) mesh.prepared = true; // 원본 정점/인덱스와 모든 LOD의 업로드 제출이 성공했을 때만 mesh.prepared를 켠다.
		}
	}
	return submitted && !uploadFailed;
}

// 모든 메시 GPU 캐시와 객체별 LOD 이력을 비운다. 다음 사용 시 원본 Scene에서 다시 준비할 수 있게 진단 상태도 초기화한다.
void Renderer::Impl::ReleaseGeometry(RHI::IDevice* device)
{
	if(device == nullptr) return; // 장치가 없으면 GPU 캐시 해제 루틴을 호출하지 않는다.
	for(auto& mesh : m_meshes) DestroyMeshState(device, mesh);
	m_meshes.clear();
	entityLods.clear();lodMeshSources.clear();lodScene.reset();
	lodDebugActive=wireframeActive=false;
}
}
