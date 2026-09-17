# shooting_game_example

2D 멀티플레이 슈팅게임. 서버(C++/Boost.Asio)를 처음부터 하나씩 직접 만들어가는 프로젝트.

서버 권위형(server-authoritative) 구조로 간다 — 방 상태, 보스 체력, 경매 결과 같은
공유 상태는 항상 서버가 판정하고, 클라이언트(Unity/C#)는 요청만 보낸다.

## 진행 상황

- [x] Step 1: TCP 접속을 받아서 로그만 찍는 서버
- [x] Step 2: 받은 데이터를 그대로 돌려주는 echo 서버
- [x] Step 3: 여러 명이 동시에 접속 가능하게 (async_accept + io_context)
- [x] Step 4: 길이-prefix + JSON 프로토콜 얹기
- [x] Step 5: 로그인 (유저네임)
- [ ] Step 6: 방 생성/참여/퇴장
- [ ] Step 7: 채팅 (방 단위 브로드캐스트)
- [ ] Step 8: 매칭 큐 + 보스 스폰
- [ ] Step 9: 보스 공격 + 클리어 리워드
- [ ] Step 10: 경매 (등록/입찰/타이머 마감)
- [ ] Step 11: 친구 (요청/수락/목록)
- [ ] Step 12: 라즈베리파이 배포 (systemd 서비스로 상시 구동)

## 프로젝트 구조

```
server/
  CMakeLists.txt
  CMakePresets.json      vcpkg 툴체인 경로 지정
  vcpkg.json             의존성 선언 (boost-asio, nlohmann-json)
  src/main.cpp           서버 전체 (Session 클래스 + main)
tools/
  test-client.ps1        PowerShell 임시 테스트 클라이언트
```

클라이언트(Unity)는 아직 없음. 서버가 어느 정도 완성된 뒤 붙일 예정.

## 프로토콜

모든 메시지는 `[4바이트 빅엔디안 길이][UTF-8 JSON 본문]` 형태로 감싸서 주고받는다.
본문 구조는 `{"type": "...", "data": {...}}`.

**Client → Server**

| type | data | 설명 |
|---|---|---|
| `Login` | `{"username": "alice"}` | 로그인. 이것만 로그인 전에 허용됨 |
| `Ping` | `{}` | 연결 확인 |

**Server → Client**

| type | data | 설명 |
|---|---|---|
| `LoginOk` | `{"playerId": 1, "username": "alice"}` | 로그인 성공 |
| `Pong` | `{}` | Ping 응답 |
| `Error` | `{"message": "login required"}` | 요청 거부. 사유를 메시지로 전달 |

## 빌드

Windows + Visual Studio + vcpkg:

1. Visual Studio에서 `server` 폴더를 "폴더 열기"로 열기
2. 구성(Configuration) 드롭다운에서 `default` 선택 (vcpkg가 의존성 자동 설치)
3. `Ctrl+Shift+B`로 빌드, `Ctrl+F5`로 실행

`Listening on port 7777...`이 뜨면 정상.

## 테스트

Unity 클라이언트가 아직 없어서, PowerShell 스크립트로 임시 클라이언트를 만들어 쓴다.

```powershell
Set-ExecutionPolicy -Scope Process -ExecutionPolicy Bypass
. .\tools\test-client.ps1
```

`Set-ExecutionPolicy`는 현재 창에만 적용되고 창을 닫으면 원복된다.
dot sourcing(`. ` 접두사)으로 불러와야 함수가 현재 세션에 남는다.

```powershell
Send-Framed $stream '{"type":"Login","data":{"username":"alice"}}'
Read-Framed $stream
```

---

# Devlog

<details>
<summary><b>Step 1 — 접속만 받아서 로그 찍기</b></summary>


**Decision:** `boost::asio::ip::tcp::acceptor`로 7777 포트를 열고,
`accept()`(동기/블로킹)로 접속을 하나씩 받아서 로그 찍고 바로 닫는 방식.

**Why:** 비동기(async_accept)부터 시작하면 io_context, 콜백, 소켓 생명주기 같은
개념이 한꺼번에 몰려서 파악하기 어려움. 제일 단순한 "acceptor가 뭔지, 소켓이 뭔지"부터
확인하고 싶어서 블로킹 버전으로 시작.

**Alternatives considered:** 처음부터 async_accept + io_context.run()으로 시작하는 것.
비동기가 결국 필요하긴 하지만(Step 3에서 도입 예정), 첫 걸음부터 개념을 겹쳐 쌓지 않기로 함.

**막혔던 부분:**
- Visual Studio로 폴더를 그냥 열면 vcpkg를 자동으로 못 찾음 →
  `CMakePresets.json`에 `CMAKE_TOOLCHAIN_FILE`을 vcpkg 경로로 직접 명시해서 해결.
- `CMake can not determine linker language` 에러 → `src/main.cpp` 파일을 오타로 찾지 못하는 문제 발생

**검증:** 새 PowerShell 창에서 `Test-NetConnection -ComputerName 127.0.0.1 -Port 7777` 실행 →
`TcpTestSucceeded : True` 확인, 서버 콘솔에 `Client connected: 127.0.0.1:...` 로그 찍힘.

</details>

<details>
<summary><b>Step 2 — echo 서버</b></summary>


**Decision:** 접속을 받은 뒤 바로 끊지 않고, `socket.read_some()`으로 데이터를 읽어서
`boost::asio::write()`로 그대로 돌려보내는 걸 클라이언트가 끊을 때까지(`EOF`) 반복.

**Why:** 소켓으로 실제 데이터를 읽고 쓰는 가장 단순한 형태를 먼저 익히고 싶어서.
아직 비동기는 안 쓰고, Step 1과 마찬가지로 한 번에 한 클라이언트만 처리하는
블로킹 방식 그대로 유지 (비동기는 Step 3에서 도입 예정).

**배운 것:** 서버가 `while(true)`라 스스로 안 끝나기 때문에, 코드 고치고 재빌드한 뒤
꼭 이전에 띄워놓은 서버 프로세스부터 꺼야 함. 안 그러면 새 프로세스가 같은 포트를
못 열어서 `Address already in use` 에러가 남.

**검증:** PowerShell에서 `System.Net.Sockets.TcpClient`로 직접 소켓을 열어서
`"hello server"` 전송 → 그대로 `"hello server"` 돌아옴 확인, 서버 로그에
`Client connected` + 수신 바이트 수 출력 확인.

</details>

<details>
<summary><b>Step 3 — 비동기로 전환 (여러 명 동시 접속)</b></summary>


**Decision:** `acceptor.accept()`(블로킹)로 한 명씩 순서대로 처리하던 구조를,
`async_accept` + `Session` 클래스(`enable_shared_from_this` 상속) 기반으로 전환.
각 연결마다 독립된 `Session` 객체가 자기 소켓/버퍼를 들고, `do_read()` ↔ `do_write()`가
서로를 콜백에서 다시 호출하며 순환하는 구조.

**Why:** 기존 구조로 직접 재현해봄 — 클라이언트 A가 접속만 해놓고 아무것도 안 보내면,
서버가 `acceptor.accept()` → 읽기 대기 루프에 갇혀서 클라이언트 B는 접속은 되어도
서버가 다시 `accept()`를 호출할 때까지 응답을 아예 못 받음(무한 대기). 여러 명을
동시에 다루려면 "한 명 처리 끝날 때까지 다음 사람을 못 받는" 구조 자체를 깨야 했음.

**Alternatives considered:** 연결마다 OS 스레드를 하나씩 새로 띄우는 방식(thread-per-connection).
구현은 더 직관적이지만, 접속자가 늘어날수록 스레드 개수가 그대로 늘어나서 메모리/컨텍스트
스위칭 비용이 커짐. `io_context` 기반 비동기는 스레드 하나로 수천 개 연결도 다룰 수 있어서
장기적으로 더 확장성 있는 선택.

**검증:** PowerShell 두 창에서 각각 `TcpClient`로 동시에 접속 → 서버 로그에 두 접속이
거의 동시에(`Client connected`) 찍힘 → 각자 다른 메시지를 보내고, 서로 막힘 없이
자기 메시지를 정확히 돌려받음 확인.

</details>

<details>
<summary><b>Step 4 — 길이-prefix + JSON 프로토콜</b></summary>


**Decision:** 모든 메시지를 `[4바이트 빅엔디안 길이][UTF-8 JSON 본문]` 형태로 감쌈.
JSON 본문은 `{"type": "...", "data": {...}}` 구조. 읽기는 `async_read_some` 대신
`boost::asio::async_read`(요청한 만큼 다 채울 때까지 반복)로 교체, 쓰기는 `std::deque`
기반 송신 큐를 통해 순서대로 전송.

**Why:** TCP는 바이트 스트림이라 "메시지 경계"를 전혀 보장하지 않음. 직접 확인해봄 —
클라이언트가 3002바이트를 한 번에 보냈는데 서버는 1024/1024/954 세 번에 나눠서 받았음.
JSON을 이런 식으로 받으면 파싱이 깨지므로, 메시지 경계를 우리가 직접 표시해야 했음.

**Alternatives considered:**
- **구분자(delimiter) 방식** (예: 줄바꿈으로 메시지 끝 표시): 구현은 더 간단하지만,
  본문에 그 구분자가 들어가면 깨짐 → 이스케이프 처리가 필요해짐. JSON 문자열 안에
  줄바꿈이 들어갈 수 있어서 부적합.
- **Protobuf**: 더 작고 빠르지만 codegen 단계가 추가됨. 스키마가 계속 바뀌는 초기
  단계에선 눈으로 읽히는 JSON이 디버깅에 유리하다고 판단. 프레이밍(길이-prefix)과
  본문 인코딩을 분리해뒀으니 나중에 본문만 Protobuf로 교체 가능.

**송신 큐를 둔 이유:** 같은 소켓에서 `async_write`가 동시에 두 개 진행되면 바이트가
뒤섞여 스트림이 깨짐. 클라이언트가 메시지 두 개를 한 번에 보내면 서버가 응답 두 개를
연달아 보내려 하므로 실제로 발생 가능한 상황이라, 큐로 직렬화함.

**방어 코드:** 길이 필드를 그대로 믿고 `resize()`하면 악의적 클라이언트가 4GB를
요청해서 메모리를 터뜨릴 수 있음. 1MB 상한 검사 추가.

**검증:**
- framed 메시지 두 개를 **하나의 TCP write로** 붙여서 전송 → 서버가 정확히 두 개로 분리해 처리 확인
- `{"type":"Ping"}` 전송 → `{"data":{},"type":"Pong"}` 수신 (양방향 왕복 성공)

</details>

<details>
<summary><b>Step 5 — 로그인</b></summary>


**Decision:** `Session`이 처음으로 게임 상태(`player_id_`, `username_`, `logged_in_`)를
갖게 됨. `handle_message`를 타입별 라우팅 구조로 바꾸고, **Login만 로그인 전에 허용,
나머지 메시지는 전부 차단**하는 게이팅 추가. 거부 사유는 `{"type":"Error"}` 메시지로 회신.

**Why:** 지금까지 서버는 "누가 보낸 메시지인지" 전혀 몰랐음. 채팅/방/경매 전부 "누가"가
전제되는 기능이라, 그 앞에 신원 확인 단계가 필요했음. Session이 연결마다 독립된 객체라
"이 연결은 누구인가"를 담기에 자연스러운 자리였음.

**Alternatives considered:**
- **플레이어 ID를 로그인 시점에 발급**: 더 자연스럽지만, ID 발급기를 Session 밖
  어딘가(중앙 레지스트리)에 둬야 해서 Session이 바깥을 참조해야 함. 아직 그럴 필요가
  없어서, 일단 접속 시점에 순번을 붙여 생성자로 넘기는 방식으로 단순하게 감.
  → 계정 시스템이나 채팅 브로드캐스트가 들어오는 시점에 중앙 레지스트리로 바뀔 예정.

**테스트 환경을 만듦:** 서버는 만들고 있는데 정작 클라이언트가 없어서, 동작을 확인할
수단 자체가 없었음. PowerShell로 프레이밍(길이-prefix)을 처리하는 임시 클라이언트를 작성.
처음엔 매번 창에 붙여넣다가 반복이 심해져서 `tools/test-client.ps1`로 분리하고,
dot sourcing(`. .\tools\test-client.ps1`)으로 불러 쓰는 방식으로 정리함.

**현재 한계 (의도적):**
- 비밀번호 없음 — 유저네임만 대면 누구든 그 이름으로 로그인됨. 외부 공개 전 반드시 보완 필요.
- 영속성 없음 — 서버 재시작하면 전부 사라짐. `player_id_`도 "계정 번호"가 아니라
  사실상 "접속 순번"에 가까움.

**검증:**
1. 로그인 전 `Ping` → `{"message":"login required"}` 에러 회신 확인
2. `Login {"username":"alice"}` → `{"playerId":1,"username":"alice"}` 회신 확인
3. 로그인 후 `Ping` → `Pong` 정상 회신 확인

</details>