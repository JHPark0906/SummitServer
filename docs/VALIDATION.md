# 검증 결과

2026-09-07에 GitHub 업로드용 소스와 검증 스크립트를 확인했다. 빌드·실행 방법은 [README](../README.md), 메시지 계약은 [PROTOCOL](../PROTOCOL.md)을 따른다.

## 검증 입력과 환경

공개 대상 29개 파일을 별도 디렉터리에 복사하고 파일별 SHA-256을 대조했다. 원본 작업 디렉터리의 빌드 산출물을 사용하지 않고, GitHub에서 받은 [ServerCore `9cc9091`](https://github.com/JHPark0906/ServerCore/commit/9cc909169f9bd7c9d7d7a536b2a821942a7ea37f)을 형제 디렉터리에 두어 빌드했다. 이 결과 문서는 검증 후 추가했으며 나머지 공개 파일은 검증한 묶음과 동일하다.

| 항목 | 환경 |
| --- | --- |
| 플랫폼 | Windows x64 |
| 생성기 | Visual Studio 18 2026 |
| CMake | 4.2.3-msvc3 |
| MSVC | 19.50.35728.0 |
| Windows SDK | 10.0.26100.0 |
| 검증 스크립트 | Windows PowerShell 5.1 |

## 빌드와 회귀 결과

[tools/VerifyBuild.ps1](../tools/VerifyBuild.ps1)을 저장소 밖의 작업 디렉터리에서 실행했다. 구성과 ServerCore 경로 인자를 생략해 Debug·Release 전체 실행 및 형제 의존성 경로를 확인했다. 서버, 부하 도구와 테스트 타깃을 모두 빌드한 뒤 각 구성의 CTest를 순서대로 실행했다.

| 구성 | 전체 빌드 | CTest | CTest 실행 시간 |
| --- | --- | --- | --- |
| Debug | 통과, 컴파일 경고 0개 | 8/8 통과 | 65.51초 |
| Release | 통과, 컴파일 경고 0개 | 8/8 통과 | 63.19초 |

총 16회가 통과했다. 등록된 검사는 부하 도구 자체 검사, backend·AOI·콘솔 회귀와 실제 TCP·UDP 소켓을 사용하는 통합 검사다. 테스트는 자체 로컬 서버를 만들고 종료하며 외부 운영 서버나 게임 에셋을 요구하지 않는다.

- 필수 CTest 8개가 모두 등록되었고 ServerCore 자체 테스트는 OFF였다.
- CMakeCache의 소스·의존성 경로가 검증 묶음을 가리켰다.
- ServerCore와 서버·도구·테스트의 실제 컴파일 명령에서 Debug `/MDd`, Release `/MD` 및 `/W4 /WX` 설정이 일치했다.
- JUnit 보고서가 `build/vs/ctest-debug.xml`, `build/vs/ctest-release.xml`에 생성되었다.
- 최종 실행 뒤 29개 입력 파일의 SHA-256이 유지되었다.

이 결과는 빌드와 회귀 검사 기록이다. 위 시간은 부하 처리량이나 네트워크 지연 측정값이 아니며 동시 접속 수·전송 빈도를 보장하지 않는다. 이번 공개 준비에서 별도의 부하 성능 실험은 실행하지 않았다. 이전 측정 보고서·JSON, 실행 파일·로그와 기존 Git 이력은 공개 묶음에 포함하지 않는다.
