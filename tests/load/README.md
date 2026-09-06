# SummitServer 부하 테스트

별도 프로젝트나 Python 설치 없이 **SummitLoadTest.exe 하나**로 여러 클라이언트를 만든다. 각 클라이언트는 TCP 제어 연결을 가지며, 기본 이동 방식인 UDP가 협상되면 UDP 소켓을 하나 더 사용한다. 기본 대상은 `127.0.0.1:17891`이며 일반 게임 서버의 `17890`과 구분한다. 도구가 서버를 자동 실행하거나 시스템 설정을 변경하지는 않는다.

## 쉽게 시작하기

1. 빌드 결과 폴더에서 `StartLoadTestServer.cmd`를 실행한다.
2. `RunLoadTest.cmd`를 실행하고 주소·포트는 Enter로 기본값을 쓴다.
3. 연결 수, 유지 시간, 종류를 입력한다. 연결 수만 확인하려면 `connect`, AOI 상태 전송을 확인하려면 `join`을 선택한다.
4. `join`/`churn`에서는 배치 유형과 각 클라이언트의 전송 빈도를 추가로 선택한다.

`RunLoadTest.cmd`의 대화형 실행도 기본 UDP 협상을 사용한다. TCP만 비교하려면 아래 CLI 예처럼 `--movement-transport tcp`를 명시한다. 서버가 UDP를 광고하지 않으면 기존 TCP로 동작하며, 광고한 서버에서 UDP가 막힌 경우에는 준비 완료로 간주하지 않는다. TCP와 UDP는 같은 숫자 포트를 사용하므로 외부 시험 시에는 두 프로토콜의 도달 가능성을 각각 확인한다.

결과는 현재 폴더의 `SummitLoadTest-result.json`에 남는다. 같은 이름으로 다시 실행하면 덮어쓰므로 비교할 때는 `--output`으로 다른 이름을 지정한다. Ctrl+C는 도구의 소켓을 정리하고 종료한다. 서버는 별도로 종료한다. 입력 EOF는 무한 대기하지 않고 종료한다.

`StartLoadTestServer.cmd`는 `127.0.0.1:17891`에 `--max-sessions 65536`을 지정한 서버를 실행한다. **65,536은 설정 상한이며 검증된 게임 참가자 수를 뜻하지 않는다.** 직접 실행할 때는 시험할 연결 수에 맞춰 용량을 지정하고, 작은 수부터 실제 연결·가입 수, 상태 빈도, 지연과 서버 자원을 함께 확인한다.

실행 중인 **서버 콘솔**에서 `/players`를 입력하면 승인된 총인원과 각 플레이어의 ID·닉네임·캐릭터를 확인한다. 가입 전 연결만 유지하는 `connect` 시나리오의 소켓은 이 목록에 나오지 않는다. `/announce 공지 내용`은 전체 공지, `/help`는 명령 도움말이다. 목록 조회는 전체 명단을 복사하고 출력하므로 부하 측정 중 반복 호출하면 측정에 영향을 줄 수 있다.

## 시나리오와 배치

| 옵션 | 동작 |
| --- | --- |
| `--scenario connect` | TCP 연결만 유지하고 Join을 보내지 않는다. |
| `--scenario join` | schema 6 가입 후 명단 페이지를 ACK하고 AOI 상태를 검사한다. |
| `--scenario idle` | 미가입 세션이 서버의 30초 기한으로 정리되는지 검사한다. |
| `--scenario churn` | 연결을 교체하며 새 ID·이름으로 재가입한다. |
| `--layout grid` | 기본 고정 격자. `--spacing 8 --columns 32`, 음수 좌표도 포함한다. |
| `--layout isolated` | 128 단위 간격. 빈 가시 집합이 정상이다. |
| `--layout crowd` | 모두 시작점 `(2.5,4)`. AOI가 밀집 비용을 없애는 것으로 해석하면 안 된다. |
| `--movement-transport udp` | 기본값. Join에서 요청하고 토큰·포트를 받으면 UdpHello/UdpReady 이후 이동을 UDP로 보낸다. 광고가 없으면 TCP로 동작한다. |
| `--movement-transport tcp` | UDP를 요청하지 않는다. 같은 서버의 TCP 이동 경로와 UDP 이동 경로를 비교할 때 사용한다. |

`grid`는 합성 분포다. `--columns`와 `--spacing`으로 열 수와 간격을 바꾸며, 같은 연결 수라도 배치에 따라 AOI의 가시 관계 수가 달라진다. 실제 게임 맵이나 플랫폼에 맞춰 움직이는 시나리오는 아니며 채널이나 시작점 특별 처리는 없다.

```powershell
# 먼저 별도 창에서 서버를 실행한다. 아래 도구 명령은 한 번에 하나씩 실행한다.
.\SummitServer.exe --address 127.0.0.1 --port 17891 --max-sessions 128 --metrics-interval-ms 1000
.\SummitLoadTest.exe --scenario connect --clients 64 --ramp 32 --hold 10 --output connect-64.json

# 각 참가자 20Hz: 작은 단계부터 올린다.
.\SummitLoadTest.exe --scenario join --clients 64 --ramp 32 --per-client-rate 20 --layout grid --hold 20 --output aoi-64.json
# 같은 배치·빈도·유지 시간으로 전송 방식만 비교한다.
.\SummitLoadTest.exe --scenario join --clients 64 --ramp 32 --per-client-rate 20 --movement-transport tcp --hold 20 --output tcp-64.json
.\SummitLoadTest.exe --scenario join --clients 64 --ramp 32 --per-client-rate 20 --movement-transport udp --hold 20 --output udp-64.json
.\SummitLoadTest.exe --scenario join --clients 64 --per-client-rate 20 --layout isolated --hold 5 --output isolated.json
.\SummitLoadTest.exe --scenario churn --clients 64 --per-client-rate 20 --hold 10 --churn-rate 5 --seed 7 --output churn.json
.\SummitLoadTest.exe --scenario idle --clients 64 --ramp 32 --hold 35 --output idle.json
.\SummitLoadTest.exe --self-check
```

`--hold`는 초기 연결·가입·명단 동기화와 협상된 UDP 준비가 모두 성공 또는 실패한 뒤부터 센다. 실패가 발생한 실행은 PASS가 아니다. `--ramp`는 초당 초기 접속 시도 수다. `--rate`는 전체 합계 전송 시도/초, `--per-client-rate`는 현재 가입자 수에 곱하는 대안이다. 둘을 동시에 지정하지 않는다. CLI에서 생략하면 전체 합계 20회/초이며, 인자 없는 대화형 `join`/`churn`은 참가자별 빈도를 묻는다. TCP는 가입 직후, UDP는 UdpReady 직후에 AOI 등록용 초기 상태를 한 번 추가 전송하고 성공한 로컬 송신을 `states_sent`에 포함한다. UDP 준비 기한도 `--join-timeout`을 사용하며 TCP 연결 완료부터 센다. UDP 연결은 TCP Heartbeat를 5초마다 보내 제어 연결의 유휴 만료를 막는다. 초기 위치 대기 기한은 별도로 유효하므로 게임과 유사한 검증에는 각 참가자 20Hz를 사용한다.

`connect`는 가입이나 heartbeat를 보내지 않으므로 서버의 30초 가입 기한을 넘겨 계속 연결을 유지하는 검사가 아니다. 그 정리를 확인하려면 `idle`을 사용한다. 비교 실행 사이에는 기존 도구가 종료되고 서버의 연결이 정리된 것을 확인하며, 한 실행의 고유 이름·ID 검증에 다른 도구나 게임 클라이언트를 섞지 않는다.

## 검증과 해석

- 실제 ServerCore 프레이밍·JSON·DatagramCodec을 사용한다. 서버 TCP 페이지·AOI 배치는 최대 32항목과 8 KiB 본문 한도다. UDP는 28바이트 헤더를 포함해 최대 1,200바이트이며 토큰이 일치해야 처리한다. 도구 TCP 수신 방어 한도는 기본 64 KiB이며 `--max-frame`으로 바꿔도 1 MiB 미만이다. 잘못된 UTF-8/JSON, 부분 TCP 프레임 EOF, schema·ID·프로필 오류는 실패다.
- ID는 정확한 10진수 문자열로 검사한다. 실행별 고유 이름과 A~E를 사용하므로 다른 실행/게임 클라이언트가 섞이면 실패한다. 격리한 서버를 사용한다.
- 전역 명단과 가시 집합을 따로 관리한다. TCP에서 Enter 없는 상태, 자기 상태, 고정 좌표의 관심 범위 밖 상태는 실패다. UDP는 TCP Enter보다 먼저 도착하거나 Exit/Left 뒤에 늦게 도착할 수 있어 미등록 상태를 `udp_untracked_state_items`로 세고 버린다. UDP가 스스로 명단이나 가시 집합을 만들지는 않는다. `join` 종료 전에는 보여야 할 주변 참가자가 빠지지 않았는지도 검사한다. 이동·히스테리시스·순간이동은 별도 서버/클라이언트 회귀에서 확인한다.
- 상태 식별자는 좌표와 분리한 `q`다. 요청의 `c:5`가 승인 캐릭터로 교체되는지도 검사한다. 공지·실제 입퇴장은 별도 집계하고 AOI Exit와 구분한다.
- UDP 상태의 `r`은 정확한 64비트 10진수 문자열로 읽고, 해당 플레이어의 Enter/최신 상태보다 클 때만 적용한다. 같은 `r`의 정지 상태 재전송과 오래된 상태는 `udp_stale_or_refresh_state_items`로 구분하고 갱신률·지연 표본에 더하지 않는다. 서로 다른 플레이어의 배치는 순서가 바뀌어도 처리하므로 패킷 순서 자체는 폐기 조건이 아니다.
- `connect_latency`는 TCP 연결 시간, `join_latency`는 연결 완료부터 JoinAccepted까지다. `ramp_settled_ms`에는 명단 완료와 UDP 준비 대기가 포함된다. 지연 표본은 최대 100,000개이며 초과분은 seed 기반 reservoir 표본이다. 표본이 없으면 `null`이다.
- `state_relay_latency`는 전체 TCP 프레임 또는 UDP 데이터그램이 로컬 send에 수락된 시점부터 수신자가 읽을 때까지다. 로컬 수락은 원격 도착 보장이 아니다. 최근 256개 `q`에 남은 늦은 상태도 포함한다. 이력 밖 관측은 `relay_history_expired`로 명시하므로 이 값이 크면 지연 분위수만으로 판단하지 않는다.
- `hold_actual_periodic_hz_per_joined_client`는 초기 상태를 제외한 유지 구간 송신 수를 가입자·초로 나눈 평균이다. `hold_actual_state_update_hz_per_visible_edge`는 실제로 더 새로운 q를 받은 횟수를 가시 관계·초로 나눈 값이다. **송신 20Hz와 원격 복제 20Hz는 다르다.** `state_schedule_missed`, `state_client_backpressure_skips`, `states_sent`도 확인한다. PASS는 연결·프로토콜·가시 집합 검증이며 목표 빈도나 지연 한도를 보장하는 판정이 아니다.
- `final_visible_source_age`는 종료 시 표시 중인 상태가 원본 클라이언트에서 송신된 뒤 지난 시간이고, `final_visible_receive_age`는 마지막 수신 뒤 지난 시간이다. 송신 이력을 확인하지 못한 관측은 `final_visible_source_age_history_missing`으로 구분한다. 초기 Enter 후 후속 상태가 실제 송신되고 충분한 관찰 시간이 지났는데도 갱신을 한 번도 받지 못하면 `missing_state_updates`로 실패한다. 오래된 값을 반복 수신하는 것을 정상 갱신으로 세지 않는다.
- `final_visible_edges`, `maximum_visible_per_client`는 최종 고정 배치의 가시 관계 수다. `state_batch_frames`와 `states_received`는 프레임과 상태 항목으로 단위가 다르다. 서버 AOI 지표와 함께 본다.
- `requested_movement_transport`와 `udp_advertised_sessions`, `udp_ready_received`, `udp_unavailable_tcp_fallback_sessions`를 함께 확인해 실제 사용한 경로를 구분한다. `udp_pending_before_cleanup`이 남으면 실패한다. `udp_send_would_block`은 보내지 못한 데이터그램 수이며 송신 이력에 성공으로 기록하지 않는다. `bytes_sent/received`에는 TCP 길이 접두사 또는 UDP 28바이트 헤더를 포함하며 IP/UDP/TCP 운영체제 헤더는 포함하지 않는다.
- 정리는 현재 송신을 최대 약 1초 마무리하고 최대 약 3초 수신을 비운다. `cleanup_*`는 검증 구간과 구분하며 새 프로토콜 검사를 하지 않는다. `remaining_local_sockets: 0`은 도구의 회수이며 서버 회수는 서버 지표로 확인한다.

한 프로세스의 비차단 WSAPoll 루프이므로 같은 PC에서 실행하면 서버와 CPU를 공유한다. 도구 자체의 수신 처리가 늦어져도 측정 지연과 UDP 유실에 영향을 줄 수 있으므로 서버 지표와 도구 CPU를 함께 본다. 명단은 O(N²) 비트 저장소이며 가시 상태·소켓은 별도 자원을 쓴다. UDP 협상 시 `--clients 1000`은 TCP 1,000개와 UDP 1,000개를 사용한다. `remaining_local_tcp_sockets`와 `remaining_local_udp_sockets`는 따로 제공된다. 재접속 이력은 262,144회, 상태 이력은 활성 송신자별 256개로 제한한다. 과도한 재접속에는 Windows 임시 포트/TIME_WAIT 한계도 영향을 준다.

서버는 관찰자당 4,096명, 전역 4,000,000개 가시 관계를 상한으로 두며 초과 연결은 명시적으로 종료한다. 가까운 사람 일부를 조용히 누락시키지는 않는다. 시작점 밀집 완화 정책은 별도 작업이다.

## 빌드

SummitServer CMake의 `SummitLoadTest` 대상이다. 결과 폴더에 실행 파일, 실행용 cmd 두 개와 이 문서(`LOAD_TEST.md`)가 배치된다. 별도 패키지는 설치하지 않는다. `--self-check`는 소켓 없이 TCP/UDP 프레이밍·JSON·정확한 64비트 ID/리비전·분위수를 확인하며 부하 검증을 대신하지 않는다. CTest의 `SummitLoadToolSelfCheck`가 같은 검사를 실행하고, `SummitServer.Udp`는 별도 임시 루프백 서버로 UDP 인증·정지 재전송·엔드포인트 교체와 기존 TCP 경로를 검사한다. 종료 코드: 성공 0, 검증 실패 1, 옵션/실행 오류 2, Ctrl+C 취소 130.
