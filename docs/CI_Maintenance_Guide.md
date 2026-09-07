# CI 운영 가이드: 예제 추가·이름 변경·검사 제외

기준: 2026-09-07, `main-merge-clean`의 `a7d5979` 커밋. 이 문서는 해당 로컬 소스를 설명하며, GitHub `main`에 이미 배포됐다는 의미는 아니다. 다른 커밋에서 작업한다면 먼저 실제 workflow와 지원 표를 확인한다.

이 문서의 JSON/YAML은 수정 방법을 보여주는 예시다. 문서를 작성하면서 CI 설정이나 예제 소스를 변경하지 않았다.

## 1. 어떤 파일을 바꾸면 되는가

| 바꾸려는 내용 | 주로 수정할 파일 | 영향 |
|---|---|---|
| CI를 언제 실행할지 | [ci.yml](../.github/workflows/ci.yml)의 `on` | 브랜치·이벤트별 전체 CI 실행 여부 |
| OS/API/Debug·Release 검사 조합 | `ci.yml`의 `jobs.build.strategy.matrix.include` | 빌드 작업의 개수와 환경 |
| 예제의 등록·이름·경로·API별 검사 | [ci-support.json](../.github/ci-support.json) | CI와 일반 로컬 빌드의 예제 선택 |
| 실제 CMake 타깃 이름·소스 목록 | 각 예제의 `CMakeLists.txt` | 실행 파일의 이름과 빌드 내용 |
| 셰이더 배치·고정 파일 이름 | [examples/CMakeLists.txt](../examples/CMakeLists.txt), 예제 CMake와 소스 | 실행 파일 옆에 배치되는 셰이더와 런타임 참조 |
| 예제 등록·셰이더 검사 공통 구현 | [ExampleSupport.cmake](../cmake/ExampleSupport.cmake) | 지원 표를 CMake 타깃으로 변환하는 방식 |
| 지원 표 형식·출력물·CPU 실행 판정 | [check-ci-support.py](../.github/scripts/check-ci-support.py) | 잘못된 설정, 파일 누락, CPU 실행 실패 판정 |
| Vulkan 실행 대상·모델·실행 시간·로그 판정 | [check-vulkan-smoke.py](../.github/scripts/check-vulkan-smoke.py)와 `ci.yml` | Ubuntu Vulkan Debug의 별도 실행 검사 |

보통 예제를 추가하거나 검사 범위를 조정할 때는 **지원 표와 해당 예제의 CMake**만 수정한다. 공통 검사 스크립트는 판정 규칙 자체를 바꿀 때 수정한다.

## 2. 현재 CI가 하는 일

```text
main에 병합 또는 직접 push
  → 지원 표와 검사 도구 자체 검증 + 환경별 빌드 작업
  → 예제 실행 파일·셰이더 결과물 확인
  → 등록된 CPU 예제 실행
  → Ubuntu Vulkan Debug에서 LoadModel 3초 실행
  → 결과 집계 및 로그 보관
```

| 환경 | 구성 | 현재 검사 |
|---|---|---|
| Windows Vulkan | Debug, Release | 등록 예제 빌드, GLSL 컴파일, RenderGraph CPU 실행 |
| Ubuntu Vulkan | Debug, Release | 위 검사. Debug는 Lavapipe Vulkan 실행 검사도 수행 |
| Windows D3D12 | Debug, Release | 등록 예제 빌드, HLSL 컴파일, RenderGraph CPU 실행 |
| macOS Metal | Debug, Release | 등록 예제 빌드, RenderGraph CPU 실행. 별도 Metal 셰이더 검사는 현재 미등록 |
| Ubuntu Null | Release | 공통 CPU 예제인 RenderGraph 빌드·실행 |

빌드 조합 9개와 지원 표 검사·결과 집계를 합쳐 11개 작업이다. 등록 예제는 Vulkan 9개, D3D12/Metal 8개, Null CI 1개이며, `LightingLab`은 현재 Vulkan에만 등록돼 있다.

`RenderGraph` 예제는 역순으로 등록한 패스를 `ShadowPass → MainForwardPass → PostProcessingPass` 순서로 정렬하는지 확인하고, CPU 콜백을 실행한다. 실제 GPU에 렌더링 명령을 제출하는 검사는 아니다.

Vulkan 실행 검사는 배치된 Duck 모델을 `LoadModel`로 3초 실행한다. validation 활성화, 오류 0건, VUID 0건, device lost 없음, 정상 종료를 요구하며 최대 120초를 기다린다. 종료 정리 중에 출력된 validation 오류도 실패로 처리한다. Lavapipe는 소프트웨어 드라이버이므로 실제 GPU 드라이버·화면 정확성·성능 검증은 별도로 해야 한다.

`tests/`의 회귀 검사와 ASan/UBSan은 현재 CI에 연결돼 있지 않다.

## 3. 상태와 검사 목록을 구분하기

지원 표의 각 항목에는 `vulkan`, `d3d12`, `metal` 세 API를 모두 적는다. 지원하지 않는 API도 항목 자체를 생략하지 않는다.

| 필드 | 의미 |
|---|---|
| `status` | 지원 상태를 설명한다. 이것만 바꿔서는 검사가 자동으로 중단되지 않는다. |
| `checks` | 실제 수행할 `build`, `shader`, `cpu` 목록 |
| `shaders` | 오프라인 검사할 셰이더의 `source`, `stage` 목록 |
| `gpu_required` | 현재 지원 표에서는 반드시 `false`. 별도 Vulkan smoke의 스위치가 아니다. |

| 상태 | 허용되는 설정 |
|---|---|
| `supported` | `build` 필수. `rendering: true`이면 `shader`도 필수 |
| `planned` | 개발·확인 중인 상태. `checks`에 적힌 검사만 수행하며 빈 목록도 허용 |
| `unsupported` | 해당 API 미지원. `checks`와 `shaders` 모두 빈 목록 |

공통 제약은 다음과 같다.

- `shader` 또는 `cpu`를 쓰려면 `build`도 포함한다.
- `shader`가 있으면 `shaders`가 비어 있으면 안 된다. `shader`를 제거하면 `shaders`도 비운다.
- 셰이더 `stage`는 `vertex`, `fragment`, `compute`이며, 확장자는 API별로 `.glsl`, `.hlsl`, `.metal`이다.
- `supported`에서 `planned`나 `unsupported`로 내릴 수 있다. 이전 커밋보다 검사 수가 줄었다는 이유만으로 거부하지 않는다.
- 현재 검사기는 고정된 필드만 허용한다. JSON에 `reason`, `skip`, `enabled` 같은 임의 필드를 추가하지 않는다. 변경 이유는 커밋 문구나 별도 작업 기록에 적는다.

## 4. 개발 중 특정 검사만 건너뛰기

### 4.1 실행 검사는 끄고 C++ 빌드는 유지

해당 API의 `checks`에서 `cpu`만 제거한다. 아래는 CPU 예제의 API 설정 예시다.

```json
{
  "status": "supported",
  "checks": ["build"],
  "shaders": [],
  "gpu_required": false
}
```

이 경우 예제를 빌드하지만 `check-ci-support.py`가 실행하지는 않는다. 렌더링 예제라면 `supported`에 필요한 `shader`를 유지하거나, 다음 절처럼 `planned`로 조정한다.

**Null CI는 세 API 모두에 `cpu`가 있는 항목만 선택한다.** `RenderGraph`의 한 API에서만 `cpu`를 빼도 Null CI에서는 그 항목이 제외된다. 현재는 공통 CPU 예제가 하나이므로 Null에서 실행할 예제가 없어질 수 있다.

### 4.2 오프라인 셰이더 검사를 줄이고 빌드만 유지

렌더링 기능 개발 중 해당 API를 다음처럼 바꾼다.

```json
{
  "status": "planned",
  "checks": ["build"],
  "shaders": [],
  "gpu_required": false
}
```

이는 지원 표에서 등록한 **추가 오프라인 셰이더 검사**를 제외한다. 일반 예제 빌드에 포함된 셰이더 처리는 남는다. 특히 `examples/CMakeLists.txt`의 `dy_example_shaders()`는 Vulkan 빌드 후 `glslc`로 셰이더를 컴파일한다. 따라서 이 설정만으로 깨진 셰이더가 있는 Vulkan 예제까지 반드시 빌드되는 것은 아니다.

`DY_CI=OFF` 역시 일반 빌드의 셰이더 처리를 전부 끄는 옵션은 아니다.

### 4.3 개발 중인 예제를 특정 API에서 완전히 제외

```json
{
  "status": "planned",
  "checks": [],
  "shaders": [],
  "gpu_required": false
}
```

해당 API에서 예제 타깃을 생성하지 않는다. 현재 구조에서는 CI뿐 아니라 **같은 API를 사용하는 일반 로컬 빌드에서도 제외**된다. 컴파일 오류가 있는 예제를 잠시 빼고 다른 예제를 개발할 때 사용할 수 있다.

아예 미지원인 API는 `status`를 `unsupported`로 표시하고 나머지는 위와 같이 비운다. 한 API만 개발한다면 그 API에 검사 목록을 넣고 나머지 두 API는 이 방식으로 유지한다.

예제가 빠져도 엔진 라이브러리 빌드와 해당 OS/API 작업 자체는 계속된다. 특정 환경 작업 전체를 빼려면 8절의 matrix를 수정한다.

### 4.4 Vulkan 실행 검사만 잠시 제외

현재 Vulkan smoke는 지원 표의 `checks`와 **별도로** workflow에 직접 등록돼 있다. `LoadModel`의 `checks`를 비우거나 `gpu_required`를 바꿔도 smoke 단계는 자동으로 생략되지 않는다.

`ci.yml`에서 `Vulkan API smoke with Lavapipe software rendering` 단계의 기존 `if`를 잠시 다음으로 바꾼다.

```yaml
if: ${{ false }}
```

`LoadModel`의 Vulkan 타깃을 빌드에서 제외하거나 삭제하는 경우에는 이 실행 단계도 함께 제외해야 한다. 그렇지 않으면 실행 파일 목록에서 `LoadModel`을 찾지 못해 실패한다.

다시 켤 때는 원래 조건을 복원한다.

```yaml
if: runner.os == 'Linux' && matrix.api == 'vulkan' && matrix.config == 'Debug'
```

소프트웨어 Vulkan 의존성 설치는 남겨도 된다. 설치도 생략하려면 `Install software Vulkan smoke dependencies` 단계의 조건을 함께 조정하고, 재활성화할 때 둘 다 복구한다. 로그 판정 자체 검사인 `check-vulkan-smoke.py --self-test`는 GPU 실행이 필요 없으므로 그대로 둘 수 있다.

이유와 복구 조건을 해당 단계 바로 위 YAML 주석 등에 남긴다. 예: “모델 배포 경로 변경 중으로 실행 검사만 임시 제외. 새 모델 경로의 로컬 실행 확인 후 복구.”

## 5. 새 예제 등록

1. `examples/09_NewExample/`에 소스와 `CMakeLists.txt`를 준비한다.
2. 실제 CMake 실행 타깃 이름을 `NewExample`로 정한다.
3. 지원 표의 `items`에 같은 타깃과 경로를 등록한다.
4. 지원할 API만 검사 목록을 채우고, 로컬에서 configure·build·검사를 수행한다.

아래는 GPU 렌더링이 없는 예제를 Vulkan 구성에서 우선 빌드해 보는 전체 지원 표 예시다. **실제 저장소에서는 기존 `items`를 지우지 말고 `NewExample` 항목만 추가한다.**

```json
{
  "version": 1,
  "items": {
    "NewExample": {
      "kind": "example",
      "target": "NewExample",
      "directory": "examples/09_NewExample",
      "requires": [],
      "arguments": [],
      "rendering": false,
      "apis": {
        "vulkan": {
          "status": "planned",
          "checks": ["build"],
          "shaders": [],
          "gpu_required": false
        },
        "d3d12": {
          "status": "unsupported",
          "checks": [],
          "shaders": [],
          "gpu_required": false
        },
        "metal": {
          "status": "unsupported",
          "checks": [],
          "shaders": [],
          "gpu_required": false
        }
      }
    }
  }
}
```

`examples/CMakeLists.txt`는 마지막에 `include(ExampleSupport)`를 호출하고, 지원 표가 `add_subdirectory()`를 결정한다. 새 예제를 별도의 하드코딩된 `add_subdirectory()` 목록에도 중복 등록하지 않는다.

렌더링 예제라면 `rendering: true`와 실제 셰이더 목록을 등록하고, 해당 예제의 공통 셋업·리소스 배치까지 구현한다. 기존 렌더링 예제를 복사했다면 고정 셰이더 이름과 모델 복사 경로도 확인한다.

`cpu`를 추가할 때는 창이나 GPU 없이 검사하고 자동 종료할 수 있는 실행 경로가 필요하다. 프로그램의 성공/실패 종료 코드가 실제 검증 결과를 반영해야 하며, Release에서 사라지는 `assert`만으로 판정하지 않는다. 현재 CPU 검사 제한 시간은 실행당 60초다.

## 6. 예제 이름·폴더·실행 인자 변경

예제의 등록 ID, CMake 타깃, 폴더 이름은 서로 다른 값이다. 예를 들어 `LoadModel`을 `ModelViewer`로 정리한다면 다음 연결을 확인한다.

| 변경 지점 | 이전 | 변경 예시 |
|---|---|---|
| 지원 표의 `items` 키 | `LoadModel` | `ModelViewer` |
| 지원 표의 `target` | `LoadModel` | `ModelViewer` |
| 지원 표의 `directory` | `examples/05_LoadModel` | `examples/05_ModelViewer` |
| 예제 CMake의 타깃 선언 | `set(EXAMPLE_NAME "LoadModel")` | `set(EXAMPLE_NAME "ModelViewer")` |
| 다른 항목의 `requires` | `LoadModel`을 참조 | 새 등록 ID로 갱신 |
| 별도 Vulkan smoke | `paths["LoadModel"]` | 새 **타깃 이름**으로 갱신 |
| 설명·스크립트·실행 경로 | 이전 이름 또는 경로 | 실제 사용하는 참조를 갱신 |

등록 ID만 바꿀 수도 있다. 이때 실제 타깃 이름이 그대로라면 smoke의 `paths` 키까지 바꾸면 안 된다. 무엇을 바꿨는지에 맞춰 참조를 수정한다.

참조 검색 예시:

```powershell
rg -n --hidden 'LoadModel|examples/05_LoadModel' .github cmake examples src README.md
```

등록 ID에는 영문·숫자·`_`·`-`·`.`을 사용할 수 있으며 첫 글자는 영문·숫자·`_`이다. 타깃 이름은 영문·숫자·`_`로 제한된다. 폴더는 `examples/` 아래의 상대 경로이고, 현재 CMake 검사는 영문·숫자·`_`·`-`·`/`만 허용한다. 공백·한글·상위 경로 `..`는 이 등록 규칙에 맞지 않는다.

폴더를 옮긴 뒤에는 configure를 다시 실행한다. 생성 파일인 `build/.../ci-targets-Debug.json`을 직접 고쳐 해결하지 않는다.

실행 인자를 바꾸는 경우:

- 지원 표의 `arguments`는 `cpu` 검사에서만 전달된다. 실행 프로그램이 실제 지원하는 옵션만 넣는다.
- CPU 실행의 작업 폴더는 실행 파일이 있는 폴더다. 상대 모델·리소스 경로는 여기에 맞춘다.
- Vulkan smoke는 `arguments`를 사용하지 않는다. 스크립트 내부의 모델 경로, `--smoke-seconds=3`, `timeout=120`을 별도로 수정한다.
- 기본 모델을 바꾸면 `Models/Duck/glTF/Duck.gltf` 참조와 예제 CMake의 모델 복사 동작을 함께 확인한다.

## 7. 셰이더·기능·예제 제거

### 셰이더 추가 또는 이름 변경

지원 표의 해당 API `shaders`에 실제 소스 경로와 단계를 등록한다. 다음은 항목 하나의 예시다.

```json
{
  "source": "Shaders/new_effect_cs.glsl",
  "stage": "compute"
}
```

`source`는 예제 폴더 기준이며 `Shaders/`로 시작해야 한다. 오프라인 출력뿐 아니라 실행 파일 옆의 셰이더도 검사하므로, 셰이더 배치 코드와 실행 시 참조하는 이름까지 맞춘다. 현재 공통 배치 코드는 `mesh_vs`, `mesh_ps` 등의 이름을 직접 사용한다. 새 이름을 지원 표에만 추가하면 배치 파일 누락으로 실패할 수 있다.

### 기존 예제에 기능 시나리오 추가

같은 실행 파일을 재사용하려면 `kind: "feature"` 항목을 사용할 수 있다. `target`과 `directory`는 소유 예제와 같아야 하며, `requires`에 소유 예제의 등록 ID를 넣는다.

기능에 검사가 등록된 API는 소유 예제에도 `build`가 있어야 한다. 기능을 `supported`로 표시하려면 해당 API의 의존 항목도 `supported`여야 한다. 다른 인자의 CPU 검사를 추가한다면 실제 실행 파일이 그 인자를 처리하도록 구현한다. 현재 지원 표에 `gpu` 검사 종류를 추가하는 방식은 지원하지 않는다.

### 예제를 빌드 목록에서 제외하거나 삭제

지원 표에서 해당 `items` 항목을 삭제하면 폴더가 남아 있어도 예제 빌드 목록에서 빠진다. 함께 확인할 사항은 다음과 같다.

- 삭제할 ID를 참조하는 `requires`와 관련 기능 항목을 함께 정리한다.
- 폴더만 먼저 삭제하면 `checks`가 비어 있어도 CMake가 “등록한 예제 폴더가 없다”고 실패한다. 지원 표 등록도 함께 삭제해야 한다.
- `LoadModel`을 제외하면 독립적인 Vulkan smoke도 함께 제외하거나 다른 실행 대상으로 바꾼다.
- 빈 `items` 자체는 허용된다. 다만 엔진 빌드와 workflow 작업은 남고, 고정된 smoke 호출은 자동으로 생략되지 않는다.

## 8. CI 전체 또는 특정 환경을 건너뛰기

### 현재 기본 실행 조건

```yaml
on:
  push:
    branches: [main]
```

이 설정을 가진 브랜치에서는 작업 브랜치 push, PR 생성·갱신, 수동 실행으로 CI가 시작되지 않는다. `main` 병합과 직접 push가 대상이다. 따라서 개발 브랜치에서 작업할 때는 별도의 skip 표시가 필요 없다.

설정은 로컬 파일을 고치는 것만으로 GitHub에 반영되지 않는다. 커밋된 workflow를 원격에 반영해야 한다. 오래된 브랜치가 예전의 “모든 브랜치 push” workflow를 가지고 있으면 그 브랜치 push에서 CI가 실행될 수 있으므로, 해당 브랜치에도 새 실행 조건을 반영해야 한다.

### 문서만 바뀐 main 반영을 지속적으로 제외

정말 런타임·빌드와 무관한 문서 변경만 자동 제외하려면 `push` 아래에 경로 조건을 추가한다. 아래는 **선택적으로 적용할 예시이며 현재 설정에는 없다.**

```yaml
on:
  push:
    branches: [main]
    paths-ignore:
      - 'docs/**'
      - '**/*.md'
```

변경된 경로가 전부 이 패턴에 해당할 때만 생략한다. C++·셰이더·CMake·workflow가 함께 바뀌면 실행된다. 문서를 빌드 입력으로 사용하는 구조로 바뀐다면 제외 패턴도 다시 검토한다. [GitHub의 경로 필터 설명](https://docs.github.com/en/actions/reference/workflows-and-actions/workflow-syntax#onpushpull_requestpull_request_targetpathspaths-ignore)

### 특정 반영 한 번만 제외

GitHub는 `push`/`pull_request`에 대해 커밋 메시지의 `[skip ci]` 표시를 지원한다. 예를 들어 문구만 고친 변경에 사용할 수 있다.

```text
문서 오탈자 수정 [skip ci]
```

현재 CI의 대상은 `main` push이므로 main에 반영되는 커밋 메시지를 확인한다. 특히 squash 병합은 메시지가 새로 만들어질 수 있어, 개발 브랜치의 메시지에만 표시했다고 충분하다고 가정하면 안 된다. 코드·빌드 설정 변경의 검증을 대신하는 방법으로 사용하지 않는다.

이 표시는 전체 workflow를 생략한다. 특정 예제 하나만 건너뛰는 기능은 아니다. 나중에 CI를 병합 필수 검사로 지정하면 생략된 검사가 Pending으로 남아 병합을 막을 수 있으므로 그 설정과 함께 검토한다. [GitHub의 실행 생략 설명](https://docs.github.com/en/actions/how-tos/manage-workflow-runs/skip-workflow-runs)

### 특정 OS/API 구성만 잠시 제외

`jobs.build.strategy.matrix.include`에서 필요한 행만 제거한다. 예를 들어 macOS 검사를 잠시 제외하려면 `api: metal`인 Debug·Release 두 행을 함께 제거한다. 다시 검증할 때 원래 행을 복원한다. 지원 표의 Metal 항목을 삭제하는 것은 아니다.

최상위 `build` 작업을 통째로 `if: false`로 만드는 것은 현재 집계 방식과 맞지 않는다. `CI Required`가 `build` 결과를 `success`로 요구하기 때문에 작업을 통째로 skip하면 집계가 실패한다. 특정 실행 단계만 건너뛰거나 필요한 matrix 행을 조정한다.

## 9. 변경 후 로컬 검증 순서

아래 PowerShell 명령은 저장소 루트에서 실행한다. Windows에서는 설치된 Python을 가리키는 `py -3`를 사용한다. Linux/macOS에서는 같은 스크립트를 `python3`로 실행한다. CMake와 선택한 백엔드의 빌드 도구·SDK가 필요하다.

먼저 설정과 검사 도구를 확인한다.

```powershell
py -3 .github/scripts/check-ci-support.py
py -3 .github/scripts/check-ci-support.py --self-test
py -3 .github/scripts/check-ci-support.py --self-test-cmake
py -3 .github/scripts/check-vulkan-smoke.py --self-test
```

JSON 형식만 통과했다고 소스 파일과 실행 결과가 보장되는 것은 아니다. 예제·타깃·경로·셰이더를 바꿨다면 configure와 빌드도 수행한다. Windows의 설치된 Visual Studio 2022/Vulkan 환경에서는 다음처럼 별도 검증 폴더를 사용할 수 있다.

```powershell
cmake -S . -B build/ci-guide-vulkan -G "Visual Studio 17 2022" -A x64 -DDY_CI=ON -DUSE_VULKAN=ON -DUSE_D3D12=OFF -DUSE_METAL=OFF
cmake --build build/ci-guide-vulkan --config Debug --parallel 4
py -3 .github/scripts/check-ci-support.py --verify-build build/ci-guide-vulkan --config Debug --api vulkan
py -3 .github/scripts/check-vulkan-smoke.py --build-dir build/ci-guide-vulkan
```

오류가 난 단계에서 멈춰 원인을 확인하고 다음 단계를 진행한다. 빌드 출력 검사기는 등록된 전체 실행 파일을 요구하므로 예제 하나만 빌드한 폴더에 전체 검증을 실행하면 파일 누락으로 실패할 수 있다.

Vulkan smoke는 Debug 실행 파일과 validation layer가 필요하다. 위 Windows 명령은 로컬 GPU로 실행하며, GitHub Ubuntu의 Lavapipe 환경과 동일하다고 보지는 않는다. Linux의 정확한 SDK·ICD·Xvfb 설정은 workflow의 설치 및 smoke 단계를 따른다.

CPU 검사만 확인하려면 GPU 백엔드를 끈 Null 구성도 사용할 수 있다.

```powershell
cmake -S . -B build/ci-guide-null -G "Visual Studio 17 2022" -A x64 -DDY_CI=ON -DUSE_VULKAN=OFF -DUSE_D3D12=OFF -DUSE_METAL=OFF -DDY_ENABLE_SIMD=OFF -DDY_ENABLE_TRACY=OFF
cmake --build build/ci-guide-null --config Release --parallel 4
py -3 .github/scripts/check-ci-support.py --verify-build build/ci-guide-null --config Release --api null
```

검사 대상을 바꿨다면 폴더에 남아 있는 예전 `.exe`만 보고 성공이라고 판단하지 않는다. 다시 생성된 `ci-targets-<Config>.json`의 목록과 검사 결과를 기준으로 확인한다.

## 10. 실패 로그를 읽는 순서

| 메시지·증상 | 우선 확인할 것 |
|---|---|
| `expected fields`, `unknown/unavailable check` | JSON 필드 누락·오타·임의 필드·지원하지 않는 검사 종류 |
| `Unknown dependency`, `Dependency cycle` | 삭제·이름 변경 후 남은 `requires`와 순환 관계 |
| `Declared example directory is missing` | 지원 표의 폴더와 해당 `CMakeLists.txt` 존재 여부 |
| `missing declared target` | 지원 표 `target`과 실제 `add_executable` 이름 |
| `CMake target inventory differs` | 현재 설정으로 다시 configure했는지, 검사 API/구성이 빌드와 일치하는지 |
| `missing offline shader output` | 지원 표의 파일명·단계와 오프라인 컴파일 결과 |
| `shader was not deployed beside the executable` | 예제의 셰이더 복사·컴파일 코드와 런타임 파일명 |
| CPU 예제의 종료 코드가 0이 아님 | `ci-checks/<Config>/<항목ID>.log`의 실제 검증 실패 |
| Vulkan smoke에서 `LoadModel`을 찾지 못함 | 타깃 이름 변경·지원 표 제외·Debug 빌드 여부 |
| smoke model 누락 | Duck 모델 경로와 예제의 모델 배포 단계 |
| validation marker 오류 | Debug/validation 설정, 로그 중복·누락, 실행·종료 중 validation 오류 |

GitHub는 `build/ci-logs`, `ci-checks`, CMake 진단과 타깃 목록을 artifact로 7일 보관한다. CPU 예제 제한은 60초, Vulkan 실행 제한은 120초, 환경별 전체 작업 제한은 30분이다.

## 11. 커밋 준비 시 확인할 범위

- 예제 이름 변경이라면 지원 표·CMake·실행 참조가 함께 바뀌었는지 확인한다.
- 검사를 줄였다면 어떤 검사를 제외했고 어떤 조건에서 다시 켤지 기록한다.
- `.gitignore`나 기존 로컬 제외 규칙을 임의로 바꾸지 않는다. 현재 로컬의 `docs/`와 `tests/`는 제외 설정에 들어 있으며 강제 추가하지 않는다.
- `git diff --cached`로 실제 커밋에 들어갈 변경을 확인한다. 로컬에 파일이 있다는 사실과 스테이징 여부는 다르다.
- 로컬에서 수정·검증한 다음 검토한다. 명시적인 요청을 받기 전에는 push하지 않는다.

```powershell
git status --short
git diff --check
git diff --cached --stat
git diff --cached
```

## 참고 소스

- 프로젝트 구현: [지원 표](../.github/ci-support.json), [workflow](../.github/workflows/ci.yml), [예제 등록](../cmake/ExampleSupport.cmake), [셰이더 배치](../examples/CMakeLists.txt), [검사기](../.github/scripts/check-ci-support.py), [Vulkan 실행 검사](../.github/scripts/check-vulkan-smoke.py)
- GitHub 공식 문서: [워크플로 실행 생략](https://docs.github.com/en/actions/how-tos/manage-workflow-runs/skip-workflow-runs), [워크플로 구문](https://docs.github.com/en/actions/reference/workflows-and-actions/workflow-syntax)
