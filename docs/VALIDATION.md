# 검증 결과

2026-09-07에 UDP 공통 처리를 ServerCore로 옮긴 SummitServer 소스를 검증했습니다. 빌드 방법은 [README](../README.md), 메시지 계약은 [PROTOCOL](../PROTOCOL.md)을 따릅니다.

## 검증 입력과 환경

ServerCore와 SummitServer의 소스를 형제 디렉터리에 배치하고 기존 빌드 산출물을 재사용하지 않았습니다. ServerCore는 `Runtime::DatagramTransport`가 포함된 소스이며, SummitUdpTransport는 게임 메시지 검증·Hello 응답·backend 전달을 담당하는 어댑터입니다. 게임 엔진이나 클라이언트 에셋은 빌드 입력에 포함하지 않습니다.

| 항목 | 확인한 값 |
| --- | --- |
| 플랫폼 | Windows x64 |
| 컴파일러 | MSVC 19.50.35728.0 |
| Windows SDK | 10.0.26100.0 |
| CMake | 4.2.3-msvc3 |
| 생성기 | Visual Studio 18 2026, x64 |
| CRT | Debug `/MDd`, Release `/MD` |


## 빌드와 회귀 결과

[tools/VerifyBuild.ps1](../tools/VerifyBuild.ps1)을 Windows PowerShell 5.1에서 실행했습니다. 구성과 ServerCore 경로 인자를 생략해 Debug·Release 전체 실행과 기본 형제 경로를 확인했습니다. 서버·부하 도구·테스트의 전체 빌드가 성공한 뒤 CTest를 순서대로 실행했습니다.

| 구성 | 전체 빌드 | CTest | CTest 실행 시간 |
| --- | --- | --- | --- |
| Debug | 통과, 경고 0개 | 8/8 | 35.63초 |
| Release | 통과, 경고 0개 | 8/8 | 32.69초 |

총 **16/16 통과**이며 실패하거나 건너뛴 검사는 없습니다. 부하 도구 자체 검사, backend·AOI·콘솔 회귀와 실제 TCP·UDP·CLI·공지 통합을 포함합니다. 테스트가 자체 loopback 서버를 만들고 종료하며 운영 서버에 접속하지 않습니다.

- schema 6과 `SMU1`·28바이트 머리·최대 1,200바이트 형식을 유지했습니다.
- 양방향 UDP와 기존 TCP, 정확한 revision, 손상·금지된 고순번 메시지 거절과 정상 입력 회복을 확인했습니다.
- 토큰 격리·재전송 거절·endpoint 재바인딩·정지 상태 재전송·TCP 종료 뒤 토큰 폐기를 확인했습니다.
- ServerCore와 서버·도구·테스트의 생성된 설정에서 `/W4 /WX`, Debug `/MDd`, Release `/MD`를 확인했습니다.
- `bcrypt` 직접 의존성을 SummitServer에서 제거하고 ServerCore의 전이 링크로 빌드했습니다.
- JUnit 결과는 `build/vs/ctest-debug.xml`과 `ctest-release.xml`에 생성됐습니다.

같은 ServerCore 소스는 별도의 독립 빌드에서 새 UDP 검사 10개를 포함한 125개 CTest를 Debug·Release 각각 통과했습니다. SummitServer 소비 빌드에서는 ServerCore 자체 테스트를 기본 OFF로 유지했습니다.

위 시간은 회귀 검사의 소요 시간이며 부하 처리량이나 네트워크 지연 측정값이 아닙니다. 이번 변경에서 별도의 부하 성능 실험은 실행하지 않았습니다.
