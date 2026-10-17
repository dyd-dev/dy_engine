렌더링 1–4단원 구현 현황

| 단원 | 기술 | 상태 |
| --- | --- | --- |
| **1. 기하·애니메이션·가시성** | glTF·FBX, geometry 공유, 인스턴싱, 골격·모프 애니메이션 | 구현 |
| | CPU/GPU frustum culling, Hi-Z occlusion, indexed LOD, indirect draw | 구현 |
| | 변형 메시 LOD, 공통 meshlet 경로, 수요 기반 스트리밍 | 미구현 |
| **2. 재질·표면 반응** | PBR·미세면 BRDF·Fresnel, normal·AO·발광, 색공간 구분 | 구현 |
| | Alpha mask·blend, 양면·unlit, 표면 정보·motion 출력 | 구현 |
| | Clearcoat, 재질 이방성, SSS | 미구현 |
| **3. 광원·그림자** | 방향광·점광원·스폿, 면적광 LTC 조명, Clustered Lighting | 구현 |
| | CSM·shadow atlas·PCF, 부드러운 그림자, 정적 그림자 캐시 | 구현 |
| | 면적광 그림자, 투과·유색 그림자 | 미구현 |
| **4. 간접광·반사·투과·출력** | HDRI·SH·GGX IBL, 다중 정적 프로브·box projection·정적 가림 | 구현 |
| | SSAO·SSGI·SSR, 전용 시간·공간 필터 | 구현 |
| | 정적 프로브·화면 공간 근사 기반 GI | 부분구현 |
| | Fresnel·굴절·흡수, 1/2층 유리 | 부분구현 |
| | 단일 산란 안개, 노출·bloom·톤 매핑·색보정·선명화 | 구현 |
| | TAA(Cinematic 기본 OFF), 1920×1080·16:9 고정 출력 | 구현 |
| | 동적 다중 반사 GI·DDGI, 임의의 중첩 매질·다층 반사 | 미구현 |
| | 다중 산란, LUT, HDR 디스플레이 출력, 시간 업스케일링 | 미구현 |

GPU 가시 개수 readback: 미구현(N/A).

실행 검증: 현재 변경의 화질·안정성·60 FPS 및 Windows DX/VK·Android/iOS 실기 실측 미확인.
