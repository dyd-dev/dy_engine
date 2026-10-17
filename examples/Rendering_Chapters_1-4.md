렌더링 1–4단원과 구현 현황

렌더링을 다음 네 단원으로 체계화한다(advanced 01–04와 별개). 이번 범위는 기하 처리부터 래스터 기반 빛 수송·출력까지이며 Ray Tracing은 제외한다. 진행상황은 현재 소스 기준이다.

**1단원: 기하·애니메이션·가시성.** 변환·투영과 래스터화·깊이 검사로 표면을 만들고 필요한 기하를 고른다. glTF·FBX 로딩, 공유 geometry·인스턴싱, 골격·모프 애니메이션, CPU 컬링과 indexed LOD가 있다. GPU frustum·LOD 선택과 Hi-Z 가림 판정 결과를 indirect draw로 소비한다. 변형 메시 LOD·공통 meshlet 경로·수요 기반 스트리밍은 남아 있다. GPU 가시 개수는 readback이 없어 N/A다.

**2단원: 재질·표면 반응.** 법선·거칠기·금속성과 Fresnel·미세면 BRDF로 빛의 반응을 정한다. PBR, normal·AO·발광 텍스처, 색공간 구분, alpha mask·blend, 양면·unlit 경로가 있다. 기본색·법선·거칠기·금속성·깊이·이동벡터를 분리해 볼 수 있다. Clearcoat·재질 이방성·SSS는 미구현이다. 에셋의 채널·법선 방향·단위와 재질 값의 정합도 필요하다.

**3단원: 광원·그림자.** 광원의 위치·방향·감쇠와 광원에서 본 가시성을 계산한다. 방향광·점광원·스폿, 면적광 LTC 근사와 Clustered 광원 목록이 있다. CSM·shadow atlas·PCF, 차폐물 탐색을 쓰는 부드러운 그림자와 정적 그림자 재사용을 구현했다. 면적광 그림자와 투명체를 통과한 유색 그림자는 남아 있다. 광량·노출·표면 반응의 관계는 장면별로 조정해야 한다.

**4단원: 간접광·반사·투과·출력.** 반사·굴절과 간접 조명을 근사하고 표시 색을 만든다. HDR 환경광, SH 확산광·GGX 반사, 다중 정적 프로브·box projection·거리 통계 가림, SSAO·SSGI·SSR와 시간·공간 필터가 있다. 프로브 가림은 정적 근사이며 동적 다중 반사 GI·DDGI는 남아 있다. 유리는 Fresnel·굴절·흡수와 1/2층을 지원하지만 임의의 중첩 매질·완전한 다층 반사는 처리하지 않는다. 단일 산란 안개, 노출·bloom·톤 매핑·색보정·선명화가 있다. 다중 산란·LUT·HDR 화면 출력·시간 업스케일링은 남아 있다. TAA는 선택 기능이며 Cinematic은 TAA OFF·1920×1080·16:9다.

기본 dyf는 장면·기하와 프레임 진행을 맡는다. Extends의 Environment·Clustered·Surface·Fog·Transparency·PostProcess 모듈은 자원·이력·패스를 소유한다. RenderGraph가 의존성과 배리어를 구성하고, RHI는 공통 자원·명령 계약을 제공하며 Backend가 API별로 실행한다. Bistro는 빛·재질·유리, Emerald는 광역 가시성·LOD, Dense Arena는 움직이는 반복 물체·인스턴싱을 보여준다.

Profiling은 카메라·게임 시각·해상도·품질을 고정하고 한 항목만 바꾸는 A/B로 사용한다. FRAME, CPU 준비·기록·제출, GPU PASS와 draw·triangle 변화를 함께 본다. GPU SPAN에는 제출 사이 공백도 포함된다. LOD의 화질 근사와 배치·캐시의 중복 제거를 구분한다. 현재 변경의 화질·안정성·60 FPS 달성은 미확인이다. Windows DX/VK와 별도 Android/iOS 브랜치의 실제 기기 결과도 따로 확보해야 한다.
