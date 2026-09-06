# SummitServer

Summit 학습용 게임의 C++20 서버다. ServerCore의 TCP 세션·JSON 봉투·직렬 JobRunner 위에서 승인된 닉네임/캐릭터, 전체 채팅·운영자 공지, 전원 프로필 명단과 AOI 가시 객체를 관리한다. 이동은 UDP를 선택한 클라이언트에 UDP로 전달하며 기존 TCP 클라이언트도 지원한다.

플레이어의 이동·점프·충돌 물리는 클라이언트에서 계산한다. 서버는 유한한 좌표와 메시지 필드를 검증하고 최신 상태를 주변 플레이어에게 복제한다. 서버 권위 이동 시뮬레이션, 계정 인증, 영속 저장은 아직 구현하지 않는다. 자세한 와이어 계약은 [PROTOCOL.md](PROTOCOL.md), 부하 프로그램 사용법은 [tests/load/README.md](tests/load/README.md)를 읽는다.

## 기능과 책임

- **프로필과 멤버십:** 승인 닉네임/캐릭터와 전원 명단을 유지하며 실제 입장·퇴장·변경을 전파한다.
- **신뢰 제어:** TCP로 가입, ACK가 있는 명단 페이지, 채팅·공지와 가시 객체 생성/삭제를 전달한다.
- **이동 복제:** 선택적 양방향 UDP와 기존 TCP를 지원하며 순번, 최신 상태 병합, 바이트 예산과 우선순위로 송신을 조절한다.
- **AOI:** 위치 기반 격자와 진입/이탈 여유 구간을 사용해 가시 객체를 전원 프로필 명단과 분리한다.
- **운영 도구:** 실행 옵션, 입퇴장/채팅 로그, `/players`·`/announce` 명령, 계측과 네이티브 부하 프로그램을 제공한다.

ServerCore는 공통 TCP/IOCP·프레이밍·JSON·세션·작업 실행을 맡고, SummitServer는 게임 메시지와 UDP 소켓을 맡는다. Summit 클라이언트가 GameEngine을 사용하지만 서버는 GameEngine·GameEditor·GameBuilder를 링크하지 않는다. GameEngine·ServerCore·Summit·SummitServer는 각각의 저장소이며, GameEditor와 GameBuilder는 GameEngine 안의 하위 프로젝트다.

## 구조와 읽을 코드

| 위치 | 책임 |
| --- | --- |
| [main.cpp](main.cpp) | CLI, Host/UDP 조립, 주기 작업, 콘솔·종료·계측 수명 |
| [SummitServerBackend](SummitServerBackend.h) | 가입·프로필·채팅, 직렬 작업, AOI와 상태 스케줄러 |
| [SummitMovementTransport](SummitMovementTransport.h) | 게임 백엔드와 이동 전송 사이의 작은 인터페이스 |
| [SummitUdpTransport](SummitUdpTransport.h) | UDP 소켓, 토큰·endpoint·패킷 순번, 제한된 수신 pump |
| [ServerConsole](ServerConsole.h) | Windows/UTF-8 콘솔 입력과 명령 처리 |
| [tests](tests) | backend/AOI/콘솔 회귀 및 실제 TCP/UDP 통합 |
| [tests/load](tests/load) | 게임 엔진 없이 실행하는 네이티브 부하 프로그램 |
| [tools](tools) | 로컬 부하 실행용 cmd 도구 |
| [docs/VALIDATION.md](docs/VALIDATION.md) | 공개 소스의 빌드·테스트 구성과 실행 결과 |

[프로토콜](PROTOCOL.md)은 필드·순서·실패·예산 계약을, [부하 도구 안내](tests/load/README.md)는 로컬 실행과 결과 해석을 설명한다. 서버와 테스트에는 게임의 이미지·글꼴·씬·오디오 에셋이 필요하지 않다.

## 빌드

서버 실행 환경은 Windows x64와 MSVC다. 아래 명령에는 CMake 3.21 이상, Ninja, C++20을 지원하는 MSVC와 Windows SDK가 필요하다. [ServerCore 소스](https://github.com/JHPark0906/ServerCore)를 기본 위치인 `../ServerCore`에 둔다. 다른 위치는 `-DSERVERCORE_SOURCE_DIR=<경로>`로 지정한다. CMake가 ServerCore를 같은 빌드에 추가해 라이브러리로 연결하므로 별도 설치는 필요하지 않다. 게임 엔진이나 게임 클라이언트는 서버 빌드의 의존성이 아니다.

MSVC x64 도구 환경을 설정한 개발자 PowerShell에서 저장소 루트로 이동해 실행한다. Ninja 생성기를 명시하므로 다른 기본 생성기 설정에 의존하지 않는다.

```powershell
cmake -S . -B build/release -G Ninja -DCMAKE_CXX_COMPILER=cl -DCMAKE_BUILD_TYPE=Release
cmake --build build/release
ctest --test-dir build/release --output-on-failure
.\build\release\SummitServer.exe --help
```

기본 배치는 `ServerCore/`와 `SummitServer/`가 같은 부모 아래에 있는 형태다. 다른 위치라면 `$serverCoreSource = (Resolve-Path ../path/to/ServerCore).Path`로 경로를 구한 뒤 첫 configure에 `"-DSERVERCORE_SOURCE_DIR=$serverCoreSource"`를 추가한다. Debug는 별도 `build/debug` 디렉터리에 `-DCMAKE_BUILD_TYPE=Debug`로 구성한다. 생성기가 다른 기존 build 디렉터리를 재사용하지 않는다. ServerCore와 실행 파일은 MSVC에서 같은 동적 CRT(`/MD`, Debug `/MDd`)를 사용한다.

최상위 빌드는 `SummitServer`, `SummitLoadTest`, backend/AOI/콘솔 테스트를 구성한다. `SUMMITSERVER_BUILD_TESTS`와 `SUMMITSERVER_BUILD_LOAD_TOOL`로 선택할 수 있다. PowerShell이 발견되면 실제 TCP/UDP 소켓을 사용하는 통합 검사도 등록한다. ServerCore 자체의 독립 테스트는 이 소비자 빌드에서 기본으로 끈다. 소스와 검증 대상의 정확한 목록은 [CMakeLists.txt](CMakeLists.txt)가 기준이다.

[CMake 프리셋](CMakePresets.json)으로 같은 작업을 실행할 수도 있다.

| Configure 프리셋 | Build·Test 프리셋 | 실행 파일 디렉터리 |
| --- | --- | --- |
| `ninja-debug` | `ninja-debug` | `build/ninja-debug` |
| `ninja-release` | `ninja-release` | `build/ninja-release` |
| `vs` | `debug`, `release` | `build/vs/Debug`, `build/vs/Release` |

예를 들어 `cmake --preset ninja-debug`, `cmake --build --preset ninja-debug`, `ctest --preset ninja-debug`를 순서대로 실행한다. `vs`는 Visual Studio 2026 C++ 도구와 해당 생성기를 지원하는 CMake가 필요하며 Ninja를 사용하지 않는다. [VerifyBuild.ps1](tools/VerifyBuild.ps1)은 Windows PowerShell 5.1에서 `vs` 구성을 만들고, 기본 8개 검사의 등록 여부를 확인한 뒤 Debug·Release 전체 빌드와 CTest를 실행한다.

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools/VerifyBuild.ps1
```

스크립트의 `-Configuration Debug` 또는 `Release`로 한 구성만 선택할 수 있다. ServerCore 위치는 `-ServerCoreSourceDir`, CMake 실행 파일은 `-CMakePath`로 지정한다. 검증 스크립트는 테스트와 부하 도구 옵션을 ON으로 설정하며, JUnit 결과는 `build/vs/ctest-debug.xml`과 `ctest-release.xml`에 남긴다.

## 실행과 접속

직접 구성한 첫 빌드 예에서는 `Set-Location build/release`로 이동한다. 프리셋을 사용했다면 위 표의 실행 파일 디렉터리로 이동한다. 아래 명령은 각각 별도 실행 예다.

```powershell
.\SummitServer.exe --port 17890
.\SummitServer.exe --port 17890 --max-sessions 1000 --metrics-interval-ms 1000
# 같은 PC 검증: 외부 인터페이스와 별도인 loopback 주소/포트
.\SummitServer.exe --address 127.0.0.1 --port 17891 --max-sessions 1000
```

기본 주소는 `0.0.0.0`, 포트는 `17890`이다. 서버는 같은 번호에 TCP와 UDP를 각각 바인딩한다. `0.0.0.0`은 모든 로컬 IPv4 인터페이스를 뜻하며 클라이언트가 접속할 주소는 아니다. 같은 PC에서는 `127.0.0.1`로 접속한다. 다른 PC에서 접속하려면 해당 서버 주소와 **TCP·UDP 두 포트 모두** 도달할 수 있어야 하며, 방화벽 허용과 NAT 환경의 포트포워딩은 운영 환경에 맞게 설정한다. `--port`를 변경하면 두 프로토콜에 같은 변경을 적용한다. 공유기의 공인 IP를 로컬 bind 주소로 지정하지 않는다. 방화벽과 공유기 설정은 서버가 자동으로 바꾸지 않는다.

| 옵션 | 기본값·범위 |
| --- | --- |
| `--address` | `0.0.0.0`, IPv4 주소 |
| `--port` | `17890`, 1..65535 |
| `--max-sessions` | 생략 시 가입 16명/연결 64개. 명시하면 두 상한을 함께 1..65536으로 설정 |
| `--metrics-interval-ms` | `0`(출력 끔), 0..3600000 |
| `--help` / `-h` | 사용법 출력 후 종료 |

최대 세션 옵션은 이번 프로세스의 용량 제한이며 저장되는 설정이나 처리량 보장이 아니다. 65,536개 세션의 8KiB 프레임 Reader 상한만 계산해도 약 512.25MiB이며 실제 연결·큐·가시 관계에는 추가 자원이 필요하다. 같은 서버 주소/포트를 공유하는 프로세스를 중복 실행하지 않는다. 정상 종료는 Ctrl+C를 사용한다. 실행 중인 사용자 서버를 빌드 도구가 자동으로 중단하거나 교체하지 않는다.

## 이동과 제어 전송

[Backend](SummitServerBackend.h)는 schema 6을 사용한다. Join에 `movementTransport:"udp"`를 넣으면 구성된 서버가 `JoinAccepted.udp {port,token}`을 반환한다. 클라이언트는 UDP Hello/Ready로 경로를 확인하고 이동을 UDP로 보낸다. 이 선택이 없는 세션은 기존 TCP 이동을 유지한다. 협상 후 UDP 장애를 자동으로 TCP 이동으로 전환하지는 않는다.

가입·명단 페이지·프로필·채팅·공지·AOI 생성/삭제는 TCP로 전달한다. UDP datagram은 `SMU1` magic, 16바이트 세션 토큰, 8바이트 big-endian 패킷 순번으로 된 28바이트 머리와 JSON payload다. 전체 상한은 1,200바이트이고 조각 재조립은 없다. TCP는 별개의 4바이트 little-endian 길이 프레임을 쓴다.

서버는 상태마다 10진수 문자열 revision `r`을 붙인다. 클라이언트는 Enter의 baseline과 ID별 최신 `r`로 중복·역순 datagram을 버리고, 가시 객체가 없는 ID의 상태로 객체를 만들지 않는다. 서버는 마지막 성공 전송 후 500ms 이상 지난 최신 상태도 재전송 후보로 삼아 유실된 마지막 정지 상태를 복구하려 한다. 예산·우선순위·네트워크 상황에 따른 best-effort이며 500ms 내 도착 보장은 아니다.

이동 UDP는 TCP의 idle 시각을 갱신하지 않는다. 가입한 클라이언트는 정지 중에도 5초마다 TCP `Heartbeat {}`를 보낸다. TCP idle 제한은 30초다. 토큰은 가입 롤백/실제 TCP 종료 때 해제한다. 현재 토큰은 암호화나 MAC를 제공하지 않으며 계정 인증과도 다르다.

실제 UDP 소켓은 [SummitUdpTransport](SummitUdpTransport.h)의 책임이다. ServerCore가 제공하는 것은 공유 `Protocol/DatagramCodec.h`와 JSON/Prepared 값 API이며, ServerCore Host에 범용 UDP 세션 기능이 추가된 것은 아니다.

## 틱 예산과 AOI

서버는 50ms 주기로 최신 상태를 복제한다. 한 변 16인 격자에서 주변 후보를 찾고 관찰자 중심 반폭/반높이 32/20 안의 대상을 Enter시킨다. 기존 가시 대상은 36/24를 벗어나야 Exit한다. 전원 닉네임/캐릭터 명단과 전체 채팅은 AOI 밖에도 유지한다. 시작 지점이나 밀집 구역을 임의로 분리하거나 가까운 플레이어 일부만 숨기지 않는다.

| 옵션 | 기본 바이트/tick |
| --- | ---: |
| `--state-bytes-per-tick` | 클라이언트당 2,048 |
| `--total-state-bytes-per-tick` | 서버 전체 1,048,576 (1MiB) |
| `--control-bytes-per-tick` | 클라이언트당 16,384 (16KiB) |
| `--total-control-bytes-per-tick` | 서버 전체 262,144 (256KiB) |

네 값의 허용 범위는 각각 1,024..16,777,216이다. 바이트는 성공적으로 수락한 **JSON envelope + TCP 4바이트 또는 UDP 28바이트 응용 머리**다. IP/UDP/TCP 커널 헤더와 TCP 재전송은 포함하지 않으므로 전체 링크 대역폭의 정확한 상한과 같지는 않다.

제어 예산은 DirectoryPage/Ready와 VisibilityEnter/Exit/Ready의 점진 전송에 적용한다. 가입·실제 입퇴장·프로필·채팅·공지 방송은 기존 bounded TCP 큐를 사용한다. 제어와 상태는 독립된 전체 예산과 수신자 순환 커서를 가지며, 예산 부족과 재시도 가능한 역압은 다음 tick으로 미룬다.

실제 좌표가 변하면 이전·새 위치 주변 관찰자를 가시성 재계산 대상으로 표시한다. 위치가 같은 반복 상태는 가시 집합을 다시 만들지 않으며, 미완료 Enter/Exit는 계속 재시도한다. 전역 방송 봉투도 한 번 준비해 수신자마다 같은 JSON을 재직렬화하지 않는다.

이동은 오래 기다린 변경, 가까운 대상과 의미 있는 변화에 우선순위를 준다. 과거 상태 패킷을 FIFO로 쌓지 않고 최신 상태를 유지하며 송신이 수락됐을 때만 해당 관계의 revision/대기를 확정한다. 최신 상태의 JSON은 Prepared 값으로 한 번 인코딩해 여러 수신자의 배치에 재사용한다. TCP 이동의 큐 관측 검사도 오래된 상태 누적을 줄인다. 상세 점수·예산·오류 계약은 [프로토콜의 스케줄러 설명](PROTOCOL.md#틱-바이트-예산과-최신-상태-스케줄러)에 있다.

50ms는 예약 주기이며 CPU가 밀리면 실제 복제 빈도는 낮아진다. 전원 명단과 전역 방송의 정보량, 밀집 AOI의 O(N²) 관계, 후보 검사와 정렬 비용은 남는다. 사용자 수용량이나 UDP의 처리량 개선은 동일한 조건의 실제 부하 검증으로 판단해야 한다.

## 운영 명령과 로그

```text
/help
/players
/announce 잠시 후 서버를 종료합니다.
```

`/players`는 승인된 전체 인원 헤더와 플레이어별 ID·닉네임·캐릭터 JSON 한 줄을 출력한다. 빈 명단은 명확히 표시한다. `/announce`는 모든 가입자에게 초록색 공지를 전달하며 UTF-8 1..512바이트, ASCII 제어문자/공백만 금지 규칙을 쓴다. 실제 입장/퇴장은 노란색 알림으로 한 번씩 전달한다. 명단의 초기 페이지는 과거 입장 알림을 만들지 않는다.

콘솔 입력 스레드는 상태를 직접 읽지 않고 bounded 작업을 Host JobRunner에 넣는다. 입력 줄은 2,048단위이고 공지/명단 조회 대기는 합계 64개다. 명단은 조회 시점의 소유 복사본이며 출력 준비 비용은 인원에 비례한다. Windows 콘솔은 한글을 지원하고, 리디렉션한 stdin은 UTF-8(BOM 허용)이어야 한다. EOF만으로 서버를 종료하지 않는다.

승인된 입장·퇴장·채팅·운영자 공지는 단일 행 JSON escape를 거쳐 이벤트 로그에 기록한다. 매 이동 입력은 로그에 넣지 않는다. 파일 보관은 실행 시 stdout/stderr 리다이렉션으로 선택할 수 있다. `--metrics-interval-ms 1000`은 세션·대기열·AOI·수락 바이트·역압·tick 시간·UDP 송수신 통계를 출력한다. `last_sample_before_stop`은 마지막 관측값이며 종료 완료 후 active=0을 증명하는 값이 아니다.

## 검증과 현재 한계

[CMakeLists.txt](CMakeLists.txt)의 CTest는 backend/AOI/콘솔 C++ 회귀와 도구 자체 검사, 실제 TCP·UDP·CLI·공지 통합을 등록한다. 기본 옵션과 PowerShell이 있는 환경에서는 8개 항목이다. 통합 검사는 임시 loopback 서버를 직접 실행하며, 별도로 게임 클라이언트나 운영 서버를 준비할 필요는 없다. 가입 원자성, 페이지 ACK, 승인 프로필, 입퇴장/채팅, 가시 수명과 최신 상태, 실제 직렬화 바이트 예산, WouldBlock 재시도, 대기 상태와 수신자 순환 공정성을 확인한다. UDP 검사는 토큰 격리·역순 입력·손상 패킷·endpoint 재바인딩·정지 상태 재전송·TCP 종료 뒤 토큰 해제를 포함한다. 실행 구성과 결과는 [검증 기록](docs/VALIDATION.md)을 확인한다.

성능을 측정하려면 [부하 도구](tests/load/README.md)로 별도 loopback 서버에 작은 연결 수부터 실행한다. 연결·가입 성공, 실제 송신과 원격 갱신 빈도, 지연, source age, 서버 CPU·메모리를 함께 기록한다. 고정 좌표의 합성 부하, 지속적인 이동, 여러 PC 사이의 네트워크 부하는 서로 다른 조건이다. CTest 통과나 최대 세션 옵션은 특정 동시 접속 수의 처리량·장기 안정성을 보장하지 않는다.

UDP는 순서·전달을 보장하지 않고 500ms 재전송도 네트워크 혼잡 제어의 대체가 아니다. 현재는 고정된 응용 예산과 상한을 적용한다. TLS/DTLS·MAC, 사용자 계정, 서버 권위 이동 물리, 영속 프로필/채팅 이력과 자동 재접속 복원은 별도 작업이다. 한 관찰자 4,096개/전체 4,000,000개 가시 관계를 넘으면 일부를 숨기는 대신 명시적으로 연결을 종료한다. 전원 명단·채팅 방송과 밀집 관계의 비용은 여전히 남는다.

## 라이선스

이 저장소의 코드는 [MIT No Attribution](LICENSE)으로 제공한다. 별도 의존성인 ServerCore의 라이선스는 해당 저장소에서 확인한다.
