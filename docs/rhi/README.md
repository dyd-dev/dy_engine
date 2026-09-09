# RHI API HTML 문서

[index.html](index.html)을 브라우저에서 직접 열면 됩니다. 서버, npm 설치, 인터넷 연결이 필요하지 않습니다.

- Visual C++ / Microsoft Learn 문서를 참고한 좌측 탐색·본문·우측 목차 구성
- 공개 클래스, 구조체, 열거형, 형식 별칭, 함수·연산자의 한국어 참조
- 멤버별 구문·매개변수·반환 값·소유권 설명과 원본 헤더 행 링크
- `/` 또는 `Ctrl+K` / `⌘K` 검색, API 목록 필터, 코드 복사, 테마 전환, 모바일 목차, 인쇄

문서 기준은 `src/Public/RHI/*.h`입니다. `src/RHI`의 기존 내부 인터페이스와 혼용하지 않습니다. 백엔드·예제 빌드 구성은 `examples/README.md`를 참고하세요. 예제의 문법 검증은 실제 실행 파일 링크나 렌더링 지원을 의미하지 않습니다.

## 문서 갱신

저장소 루트에서 Python 3.10 이상으로 실행합니다. 외부 Python 패키지는 사용하지 않습니다.

```sh
python3 docs/rhi/generate.py
python3 docs/rhi/generate.py --check
```

`generate.py`는 현재 공개 헤더에서 구문, 공개 멤버, 기본값, 선언 행을 추출합니다. 설명은 `reference.json`, 화면과 동작은 `assets/docs.css`, `assets/docs.js`, `assets/theme.js`에서 편집합니다. 시작 예제는 `examples/clear-frame.cpp`이며, 클래스별 예제도 `examples/`에 보관합니다.

생성된 HTML, `assets/search-index.js`, `manifest.json`은 함께 보관합니다. 생성기 대신 이 파일을 직접 수정하면 다음 갱신 시 덮어씁니다. 생성기는 일반적인 C++ 파서가 아닌 이 저장소 공개 헤더의 선언 형태를 대상으로 합니다. 새 API나 멤버의 설명이 없으면 오류로 중단하므로 `reference.json`도 갱신하세요. `--check`는 헤더와 생성 결과가 다를 때 실패합니다.

예제의 공개 API 문법 검사:

```sh
g++ -std=c++17 -Wall -Wextra -Werror -pedantic -Isrc/Public -fsyntax-only docs/rhi/examples/*.cpp
```

HTTP로 확인하려면 다음 명령 후 `http://localhost:8000`을 엽니다.

```sh
python3 -m http.server 8000 --bind 127.0.0.1 --directory docs/rhi
```

화면 구성 참고: [Microsoft Learn — vector class](https://learn.microsoft.com/en-us/cpp/standard-library/vector-class?view=msvc-170). API 설명은 저장소 소스에 맞춰 작성했습니다.
