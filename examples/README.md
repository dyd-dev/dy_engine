# Examples

Graphics와 RHI의 같은 번호 예제는 같은 주제와 장면을 표현합니다. Graphics는
Canvas·Scene·Camera를 RHI 리소스와 명령으로 변환하고, RHI 예제는 그 명령을 main에 직접 구성합니다.
GPU에 그리는 경로는 공개 `RHI::IDevice` / `RHI::ICommandList` 하나입니다.

## 독립적인 폴더

각 `Graphics/01_Primitives` … `09_Text`, `RHI/01_Primitives` … `09_Text` 폴더에
`main.cpp`, `CMakeLists.txt`, `Shaders/`가 있습니다. 다른 예제나 Common을 읽지 않습니다.

- 모든 Shaders 폴더에 GLSL, HLSL, Metal 소스를 포함합니다. 셰이더 include도 해당 폴더 안에서 해결합니다.
- Graphics main도 자신의 컴파일된 셰이더를 `RendererDesc::canvasShaders` 또는 `meshShaders`에 전달합니다.
- Image·MeshMaterial의 이미지는 각각 `Assets/`에 있습니다.
- 두 ModelAnim 폴더에는 각각 Shiba FBX와 Fox GLB 한 쌍 및 필요한 텍스처만 있습니다.
  GLB는 glTF의 바이너리 컨테이너입니다. 현재 애니메이션 장면은 Fox를 사용합니다.
- Text는 `DY_EXAMPLE_FONT`로 사용자가 제공하는 TrueType 폰트를 지정합니다. 임의의 시스템 폰트를 자동 선택하지 않습니다.

FBX와 glTF는 **같은 모델의 변환본이 아니라 기존 Shiba와 Fox를 하나씩** 남긴 구성입니다.
각 예제 폴더를 독립적으로 복사할 수 있도록 두 ModelAnim 폴더에 같은 자산 쌍을 각각 보관합니다.
이미지·모델의 기존 출처 및 라이선스 파일은 자산과 함께 유지합니다.

## 빌드

전체 예제의 Vulkan 빌드:

```sh
cmake -S . -B build-vulkan -DUSE_VULKAN=ON -DDY_BUILD_EXAMPLES=ON
cmake --build build-vulkan
```

예제 하나를 저장소 밖으로 복사한 후에도 엔진 위치만 지정하면 빌드할 수 있습니다.

```sh
cmake -S /path/to/07_ModelAnim -B /path/to/build-model \
  -DDY_ENGINE_SOURCE_DIR=/path/to/dy_engine -DUSE_VULKAN=ON
cmake --build /path/to/build-model --target RhiModelAnim
```

Windows에서는 `-DUSE_D3D12=ON`과 dxc, macOS에서는 `-DUSE_METAL=ON`과
Xcode Metal 도구를 사용합니다. Vulkan은 glslc가 필요합니다. 네이티브 백엔드는 하나만 선택합니다.
main에는 API별 분기가 없고, 일반 프로젝트 모듈 `cmake/CompileShaders.cmake`가
현재 백엔드의 소스를 컴파일해 바이트와 entry point만 있는 헤더를 생성합니다.
다른 언어 소스가 누락되어도 구성 단계에서 실패합니다. Null은 네이티브 셰이더나 픽셀을
만들지 않으므로 예제 실행용이 아닙니다.

## 코드와 검증의 경계

창과 이벤트 루프는 main에서 `Platform::Window`로 직접 제어합니다. 애니메이션은 실제 경과 시간을 사용합니다.
예제 전용 Window·Rhi3D·캡처 실행기, 프레임 수 제한, 고정 시간 간격, 자동 종료를 사용하지 않습니다.
장면별 카메라·조명·재질 값은 main에 남습니다.

공개 Mesh·Model·Font·Texture API는 CPU 데이터를 제공하고, ResourceScope는 명시적으로
넘긴 RHI 핸들의 수명만 관리합니다. 리소스 생성·업로드·barrier·draw를 대신하는 예제 헬퍼는 없습니다.
Graphics의 내장 셰이더는 일반 라이브러리 사용자를 위한 기본값이며 예제 셰이더의 경로로 사용되지 않습니다.

`ctest`는 CPU 자산·폰트·수명 검사를 실행합니다. Null 구성에서는 Graphics가 조합한
Canvas, 다섯 종류의 조명, shadow, 모델 스키닝 명령을 RHI 계약으로 검증합니다.
`-DDY_TEST_NATIVE_RENDERING=ON`은 별도의 테스트 실행 파일에서 네이티브 창을 열어
텍스처·블렌딩·클리핑의 Graphics/RHI 픽셀 일치 및 스키닝 프레임 변화를 검사합니다.
이 테스트의 시간 제한·캡처는 예제 코드에 들어가지 않습니다.

현재 구성은 message.txt의 기본 도형·이미지·텍스트·3D 장면·모델 스키닝 범위입니다.
멀티스레딩, 레이트레이싱 등 후순위 기능의 완료나 세 플랫폼 전체의 픽셀 동일성을 뜻하지 않습니다.
