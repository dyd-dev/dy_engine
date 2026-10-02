# dy_engine API 문서

[문서 홈](../index.html)을 브라우저에서 직접 열면 됩니다. 서버·npm 설치·인터넷 연결 없이 검색, 코드 복사, 테마 전환과 인쇄를 사용할 수 있습니다. MSVC Docs를 참고한 좌측 API 탐색, 가운데 본문, 우측 페이지 목차 구조입니다.

## 문서 내용

- 빠른 시작: CMake 구성, 기본 렌더러와 Canvas 예제.
- 모듈 안내: 기본 API, RHI, Model 확장의 진입점과 사용 범위.
- RHI 가이드: 명령 기록·제출·표시 흐름과 리소스 소유권.
- `확장: Model`: 선택 빌드와 모델·애니메이션 입력.
- API 참조: 현재 `src/Public`에서 추출한 선언, 공개 멤버, 기본값, 원본 주석과 선언 행.
- 주요 API 사용법: 구현과 예제로 확인한 한국어 설명, 매개 변수, 반환값과 수명 규칙.

API 탐색은 기본 API(dyf), RHI, Math, Platform, 확장: Model의 다섯 모듈 아래에 기능별 그룹을 두고 각 API를 배치합니다. 현재 API가 속한 모듈과 그룹은 자동으로 펼쳐집니다. 홈은 가이드와 모듈별 API 목록으로 이동하는 개요입니다. 검색 결과와 좌측 탐색에서는 각 API의 상세 페이지로 이동합니다. 조건부 선언과 Model 확장도 포함하므로 사용 시 가이드의 빌드 조건을 확인하세요.

특정 타입에 관련된 자유 함수와 연산자는 해당 타입 페이지의 `관련 함수` 또는 `연산자`에 함께 표시합니다. C++ 멤버로 변경하는 것은 아니며, 네임스페이스 함수의 원형과 선언 위치를 유지합니다. `operator*`, `Dot`, `Normalize`, `TransformPoint`처럼 타입에 따라 의미가 달라지는 오버로드는 각 타입 페이지로 나눕니다. `ToIndex`, `IsValid`, `DecodeUtf8`처럼 한 타입에 속하지 않는 함수는 독립 항목으로 유지합니다.

## 폴더 구성

```text
dy_engine/
├─ src/Public/                      # 문서 입력
└─ docs/dy_engine_docs/
   ├─ index.html                    # 브라우저 진입점
   └─ component/
      ├─ generate.py                # 문서 생성기
      ├─ parser.py                  # C++ 선언 추출
      ├─ frontend_reference.json
      ├─ rhi_reference.json         # 수작업 API 설명
      ├─ api_groups.json            # 모듈별 기능 그룹과 API 순서
      ├─ api_owners.json            # 자유 함수·연산자를 함께 읽을 타입 페이지
      ├─ guides.json                # 사용 가이드
      ├─ examples/                  # C++ 예제
      ├─ assets/                    # CSS, JS, 검색 데이터
      └─ manifest.json              # 입력 해시와 생성 페이지 목록
```

## 갱신과 검증

Python 3.10 이상을 사용합니다. 외부 Python 패키지는 필요하지 않습니다. 저장소의 `docs/dy_engine_docs` 폴더에서 실행합니다.

```sh
python component/generate.py
python component/generate.py --check
python -m unittest discover -s component -p 'test_*.py'
python component/verify.py
```

기본 엔진 위치는 문서 폴더를 포함하는 저장소 루트(`../..`)에서 찾습니다. 문서 폴더를 저장소 밖에 별도로 배치한 경우에는 나란히 있는 `../dy_engine`을 확인합니다. 다른 위치라면 엔진 루트를 명시합니다. 생성 결과는 작업 디렉터리와 관계없이 생성기 옆의 문서 폴더에만 기록합니다.

```sh
python component/generate.py --project-root D:/dev/dy_engine
python component/generate.py --project-root D:/dev/dy_engine --check
```

기본 입력은 `src/Public`입니다. 다른 헤더 집합은 `--source-dir`로 지정합니다. 상대 입력 경로는 엔진 루트 기준이며, 이 경우 프로젝트 전용 수작업 설명은 적용하지 않습니다.

```sh
python component/generate.py --project-root D:/dev/dy_engine --source-dir src
```

생성 HTML을 직접 편집하면 다음 갱신에서 덮어씁니다. API 설명은 `frontend_reference.json`, `rhi_reference.json`, 가이드는 `guides.json`, 좌측 기능별 분류와 나열 순서는 `api_groups.json`을 수정하세요. 각 독립 API는 소속 모듈의 한 그룹에만 배치합니다. 타입 페이지에 포함된 함수는 좌측 탐색에 별도 항목을 만들지 않습니다. 분류의 누락·중복·잘못된 모듈이나 존재하지 않는 API·멤버를 지정하면 생성이 실패합니다.

`api_owners.json`은 함수의 symbol key를 소유 타입의 symbol key에 연결합니다. 모든 오버로드를 같은 페이지에 두면 문자열 하나를, 오버로드별 페이지가 다르면 후보 타입 key의 배열을 지정합니다. 배열을 쓸 때는 각 선언의 타입명 토큰과 일치하는 후보가 정확히 하나여야 합니다. 헤더와 소유 페이지가 다른 모듈에 있더라도 함수의 원래 선언 위치는 유지합니다. 예를 들어 Model 확장의 `LoadMesh`는 `MeshData`, `ToString(MaterialTextureKind)`은 `MaterialTextureKind` 페이지에서 읽습니다.

`--check`는 헤더·설명·가이드·분류·함수 소유 관계에서 다시 만든 결과와 저장된 문서를 비교하며 파일을 쓰지 않습니다. 오래된 파일 중 생성기 표시가 있는 HTML만 정리하고 수동 작성 파일은 보존합니다.

모든 텍스트는 UTF-8 without BOM으로 읽고 저장합니다.

## 예제 확인

문서 폴더에서 다음 명령으로 공개 헤더 사용을 확인할 수 있습니다.

```sh
g++ -std=c++17 -Wall -Wextra -Werror -I../../src/Public -fsyntax-only component/examples/quickstart-canvas.cpp component/examples/quickstart-rhi.cpp component/examples/clear-frame.cpp component/examples/resource-scope.cpp component/examples/render-graph.cpp
```

문서 폴더를 저장소 밖에 둔 경우 `-I` 경로를 실제 엔진의 `src/Public`로 바꿉니다. 문법 검사만으로 링크·GPU 실행·화면 결과를 확인할 수는 없습니다. 실행하려면 빠른 시작의 CMake 설정으로 엔진과 연결합니다. Apple은 Metal, Linux는 Vulkan을 자동 선택하며, Windows에서는 D3D12 또는 Vulkan을 지정합니다. `verify.py`는 생성된 HTML의 링크·앵커, 검색 링크와 인코딩을 검사합니다.

## 설명의 범위

선언·기본값·헤더 주석은 자동 추출하고 사용법은 수동 작성합니다. C++ 파서는 조건부 컴파일 활성 여부, 매크로 확장, 상속된 멤버와 외부 SDK 타입을 해석하지 않습니다. 분류하지 못한 선언은 해당 헤더와 manifest의 diagnostics에 표시합니다. API 이름 검사는 의미 변경까지 감지하지 않으므로 구현 변경 시 사용법도 검토해야 합니다.

## 로컬 미리보기

```sh
python -m http.server 8000 --bind 127.0.0.1 --directory .
```

[로컬 문서 열기](http://127.0.0.1:8000)
