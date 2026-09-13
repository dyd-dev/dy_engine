# dy_engine API 문서

[문서 홈](../index.html)을 브라우저에서 직접 열면 됩니다. 서버·npm 설치·인터넷 연결 없이 검색, 코드 복사, 테마 전환과 인쇄를 사용할 수 있습니다. MSVC Docs를 참고한 좌측 API 탐색, 가운데 본문, 우측 페이지 목차 구조입니다.

## 문서 내용

- 빠른 시작: CMake 구성, 기본 렌더러와 Canvas 예제.
- 모듈 안내: 기본 API, RHI, Model 확장의 진입점과 사용 범위.
- RHI 가이드: 명령 기록·제출·표시 흐름과 리소스 소유권.
- Model 확장 가이드: 선택 빌드와 모델·애니메이션 입력.
- API 참조: 현재 `src/Public`에서 추출한 선언, 공개 멤버, 기본값, 원본 주석과 선언 행.
- 주요 API 사용법: 구현과 예제로 확인한 한국어 설명, 매개 변수, 반환값과 수명 규칙.

검색 결과와 좌측 API 탐색은 개별 페이지로 이동합니다. 홈에서는 모든 헤더의 API를 연속해서 읽을 수도 있습니다. 조건부 선언과 Model 확장도 포함하므로 사용 시 가이드의 빌드 조건을 확인하세요.

## 폴더 구성

```text
상위 폴더/
├─ dy_engine/                 # 문서 입력
│  └─ src/Public/
└─ dy_engine_docs/
   ├─ index.html             # 브라우저 진입점
   └─ component/
      ├─ generate.py         # 문서 생성기
      ├─ parser.py           # C++ 선언 추출
      ├─ frontend_reference.json
      ├─ rhi_reference.json  # 수작업 API 설명
      ├─ guides.json         # 사용 가이드
      ├─ examples/           # C++ 예제
      ├─ assets/             # CSS, JS, 검색 데이터
      └─ manifest.json       # 입력 해시와 생성 페이지 목록
```

## 갱신과 검증

Python 3.10 이상을 사용합니다. 외부 Python 패키지는 필요하지 않습니다. `dy_engine_docs` 폴더에서 실행합니다.

```sh
python component/generate.py
python component/generate.py --check
python -m unittest discover -s component -p 'test_*.py'
python component/verify.py
```

기본 엔진 위치는 문서 폴더와 나란히 있는 `../dy_engine`입니다. 다른 위치라면 엔진 루트를 명시합니다. 생성 결과는 작업 디렉터리와 관계없이 생성기 옆의 문서 폴더에만 기록합니다.

```sh
python component/generate.py --project-root D:/dev/dy_engine
python component/generate.py --project-root D:/dev/dy_engine --check
```

기본 입력은 `src/Public`입니다. 다른 헤더 집합은 `--source-dir`로 지정합니다. 상대 입력 경로는 엔진 루트 기준이며, 이 경우 프로젝트 전용 수작업 설명은 적용하지 않습니다.

```sh
python component/generate.py --project-root D:/dev/dy_engine --source-dir src
```

생성 HTML을 직접 편집하면 다음 갱신에서 덮어씁니다. API 설명은 `frontend_reference.json`, `rhi_reference.json`, 가이드는 `guides.json`을 수정하세요. 존재하지 않는 API나 멤버 이름을 설명에 지정하면 생성이 실패합니다. `--check`는 헤더·설명·가이드에서 다시 만든 결과와 저장된 문서를 비교하며 파일을 쓰지 않습니다. 오래된 파일 중 생성기 표시가 있는 HTML만 정리하고 수동 작성 파일은 보존합니다.

모든 텍스트는 UTF-8 without BOM으로 읽고 저장합니다.

## 예제 확인

문서 폴더에서 다음 명령으로 공개 헤더 사용을 확인할 수 있습니다.

```sh
g++ -std=c++17 -Wall -Wextra -Werror -I../dy_engine/src/Public -fsyntax-only component/examples/quickstart-canvas.cpp component/examples/quickstart-rhi.cpp component/examples/clear-frame.cpp component/examples/resource-scope.cpp component/examples/render-graph.cpp
```

문법 검사만으로 링크·GPU 실행·화면 결과를 확인할 수는 없습니다. 실행하려면 빠른 시작의 CMake 설정으로 엔진과 연결하고 네이티브 백엔드를 선택하세요. `verify.py`는 생성된 HTML의 링크·앵커, 검색 링크와 인코딩을 검사합니다.

## 설명의 범위

선언·기본값·헤더 주석은 자동 추출하고 사용법은 수동 작성합니다. C++ 파서는 조건부 컴파일 활성 여부, 매크로 확장, 상속된 멤버와 외부 SDK 타입을 해석하지 않습니다. 분류하지 못한 선언은 해당 헤더와 manifest의 diagnostics에 표시합니다. API 이름 검사는 의미 변경까지 감지하지 않으므로 구현 변경 시 사용법도 검토해야 합니다.

## 로컬 미리보기

```sh
python -m http.server 8000 --bind 127.0.0.1 --directory .
```

[로컬 문서 열기](http://127.0.0.1:8000)
