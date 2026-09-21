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
- [x] Step 6: 방 생성/참여/퇴장
- [x] Step 7: 채팅 (방 단위 브로드캐스트)
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
| `RoomCreate` | `{"name": "Boss Room"}` | 방 생성 후 자동 입장 |
| `RoomJoin` | `{"roomId": 1}` | 방 참여 |
| `RoomLeave` | `{}` | 현재 방에서 나가기 |
| `RoomList` | `{}` | 방 목록 조회 |
| `ChatSend` | `{"text": "hello"}` | 현재 방에 채팅. 방에 있어야 하고 500자까지 |

**Server → Client**

| type | data | 설명 |
|---|---|---|
| `LoginOk` | `{"playerId": 1, "username": "alice"}` | 로그인 성공 |
| `Pong` | `{}` | Ping 응답 |
| `Error` | `{"message": "login required"}` | 요청 거부. 사유를 메시지로 전달 |
| `RoomState` | `{"id": 1, "name": "Boss Room", "members": [1, 2]}` | 방 현재 상태. **요청자뿐 아니라 방 전원에게 전송** |
| `RoomListResult` | `[{"id": 1, ...}, ...]` | 방 목록 (배열) |
| `RoomLeaveOk` | `{}` | 퇴장 완료 (나간 본인에게만) |
| `ChatBroadcast` | `{"fromId": 1, "fromName": "alice", "text": "hello"}` | 같은 방 전원에게 채팅 전달 |

`RoomState`는 요청에 대한 응답이 아니라 **서버가 먼저 미는 메시지**다. 방 생성/참여/퇴장,
그리고 누군가 연결이 끊겼을 때 방에 남은 전원이 받는다. 클라이언트는 자기가 요청하지 않은
메시지도 언제든 도착할 수 있다고 가정해야 한다.

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

| 함수 | 용도 |
|---|---|
| `New-Client` | 연결 하나를 열고 스트림을 돌려줌. 여러 번 불러서 한 창에서 여러 명을 흉내낼 수 있음 |
| `Send-Framed` | 길이-prefix를 붙여 전송 |
| `Read-Framed` | 한 개를 읽음. 올 때까지 블로킹 |
| `Read-Available` | 지금 도착해 있는 것만 전부 읽음. 브로드캐스트 확인용 |
| `Close-Clients` | 열어둔 연결 전부 닫기 |

```powershell
$alice = New-Client
$bob   = New-Client

Send-Framed $alice '{"type":"Login","data":{"username":"alice"}}'
Send-Framed $bob   '{"type":"Login","data":{"username":"bob"}}'
Read-Available $alice
Read-Available $bob

Send-Framed $alice '{"type":"RoomCreate","data":{"name":"Boss Room"}}'
Send-Framed $bob   '{"type":"RoomJoin","data":{"roomId":1}}'
Read-Available $alice     # bob이 들어온 걸 alice도 통보받는다

Send-Framed $alice '{"type":"ChatSend","data":{"text":"bob 왔냐"}}'
Read-Available $bob
```

`Read-Framed`는 메시지가 올 때까지 멈춰 있어서 브로드캐스트 확인에 쓰기 어렵다.
서버가 언제 몇 개를 밀어줄지 모르기 때문에, 도착해 있는 것만 꺼내는 `Read-Available`을 쓴다.

스크립트 파일은 **UTF-8 with BOM**으로 저장해야 한다. Windows PowerShell 5.1은 BOM이 없으면
`.ps1`을 시스템 ANSI 코드페이지(한국어 환경이면 949)로 읽어서, 한글이 들어간 경로나 문자열이 깨진다.

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

<details>
<summary><b>Step 6 — 방 생성/참여/퇴장</b></summary>

**Decision:** `Server` 클래스를 도입해 공유 상태(방 목록)와 접속 수락을 맡기고,
`Session`은 `Server&`를 참조해 방 요청을 위임. 방은 `Room{id, name, members}` 구조체로,
`Server`가 `unordered_map<uint32_t, Room>`으로 보관.

**Why:** 지금까지 모든 상태는 Session 안에 있었고 그게 맞았음 — 소켓/유저네임/로그인 여부는
전부 "그 연결만의 것"이니까. 그런데 방 목록은 **모두가 공유하는 상태**라 Session 안에 둘 수 없음.
alice의 Session에 방 목록을 넣으면 bob이 그걸 볼 방법이 없음. 그래서 모든 Session 바깥에
중앙 관리자가 필요해졌음.

**순환 참조 문제:** Session은 Server를 알아야 하고(방 요청), Server도 Session을 알아야 함(생성).
C++은 위에서 아래로 읽으므로 둘 다 먼저 쓸 수 없음.
→ `class Server;` 전방 선언으로 Session이 `Server&` 멤버를 갖게 하고, 실제로 Server를 호출하는
함수들은 **클래스 안에선 선언만, 정의는 Server 뒤로** 미뤄서 해결. 참조 멤버는 대상의 내용을
몰라도 선언 가능하다는 점을 이용한 것. 헤더/소스 분리가 필요해지는 이유를 한 파일 안에서
미리 겪은 셈.

**리팩터링 먼저 (6a):** 방 기능을 붙이기 전에 accept 루프를 `Server::do_accept()`로 먼저 옮김.
부수 효과로 `std::function<void()> do_accept` 자기참조 꼼수가 사라짐 — 멤버 함수는 자기 이름을
그대로 호출할 수 있어서. 캡처도 `[&]` → `[this]`로 줄고, `main()`은 5줄이 됨. 이 단계에서 기능은
하나도 바꾸지 않고 Step 5 테스트를 그대로 재실행해 동작이 동일함을 확인한 뒤 6b로 넘어감.

**설계 판단:**
- `room_id_ = 0`을 "어느 방에도 없음"으로 사용. 방 번호를 1부터 발급해 0을 sentinel로 씀.
- 한 번에 한 방만 — 이미 방에 있으면 생성/참여 거부.
- 빈 방은 자동 삭제. 안 그러면 아무도 없는 방이 목록에 계속 쌓임.
- 멤버는 `player_id`만 저장. username까지 보여주려면 Session 목록이 필요한데, 그건
  Step 7(채팅 브로드캐스트)에서 진짜로 필요해질 때 추가 예정.

**소멸자 대신 명시적 정리:** 연결이 끊기면 방에서 빼야 하는데, `~Session()`에서
`server_.leave_room()`을 호출하면 위험. 프로그램 종료 시 Server가 먼저 파괴되고 Session이
나중에 파괴되면 이미 죽은 Server를 참조하게 됨. 그래서 읽기 에러 핸들러에서 `on_disconnect()`를
명시적으로 호출하는 방식으로 함.

**검증:**
1. alice가 `RoomCreate` → `{"id":1,"members":[1],"name":"Boss Room"}` 수신
2. **bob이 `RoomList` → alice가 만든 방이 보임** (공유 상태 동작 확인)
3. bob이 `RoomJoin` → `{"id":1,"members":[1,2]}` — 멤버 2명으로 증가
4. alice 연결 종료 → bob이 `RoomList` → `members:[2]`로 자동 정리됨 확인

</details>

<details>
<summary><b>Step 7 — 채팅 (방 단위 브로드캐스트)</b></summary>

**Decision:** `Server`에 `unordered_map<uint64_t, shared_ptr<Session>> sessions_`(세션 레지스트리)를 두고,
로그인 시점에 등록 / 연결 종료 시점에 제거. `broadcast_to_room()`이 방 멤버의 `player_id`를 돌면서
이 표로 실제 연결을 찾아 전송. 프로토콜에 `ChatSend` → `ChatBroadcast` 추가.

**Why:** 방은 멤버를 `player_id`로만 들고 있다(Step 6의 결정). 그래서 "2번 플레이어에게 보내라"를
실행할 방법이 없었음 — 번호는 알지만 그 번호가 어느 연결인지 모름. 번호와 연결을 잇는 표가
Session 바깥에 필요해졌음. Step 6에서 방 목록 때문에 `Server`를 만든 것과 정확히 같은 이유.

**Alternatives considered:**
- **`Room`이 `Session` 포인터를 직접 보관**: 브로드캐스트만 보면 제일 짧음. 그런데 방 목록 JSON을
  만들 때마다 Session을 끌고 다녀야 하고, 무엇보다 **방 밖으로 보내는 메시지에는 쓸 수 없음**.
  경매 낙찰 알림(Step 10), 친구 요청 도착(Step 11)은 같은 방 사람이 아니다. 레지스트리는 방과
  무관하게 재사용되므로 이쪽을 택함.
- **`weak_ptr`로 보관**: 제거를 깜빡해도 메모리가 새지 않는 안전장치가 됨. 대신 보낼 때마다
  `.lock()`으로 살아있는지 확인해야 하고, 죽은 항목이 표에 계속 남음. Step 6에서 이미 "연결이
  끊긴 시점에 명시적으로 정리한다"를 규칙으로 정했으므로 같은 규칙을 적용.

**소유권 주의:** `sessions_`가 `shared_ptr`로 들고 있으니, `on_disconnect()`에서 빼지 않으면
소켓이 닫혀도 Session 객체가 영원히 안 죽는다. 반대로 `erase`하는 순간 참조 카운트가 0이 되어
자기 자신이 파괴될 수도 있는데, `on_disconnect()`는 async 핸들러 람다 안에서 호출되고 그 람다가
`self`(shared_ptr)를 붙잡고 있어서 함수가 끝날 때까지는 안전하다.

**등록 시점을 로그인으로 잡은 이유:** 접속 시점에 등록하면 이름도 없는 연결이 표에 들어간다.
아직 누구인지 모르는 상대에게 보낼 메시지는 없음. 부수 효과로 `handle_login`이 `Server`를
호출하게 되면서, Step 6에서 만든 "클래스 안엔 선언만, 정의는 Server 뒤" 그룹에 합류했다.

**같이 드러난 버그:** 지금까지 `RoomCreate`/`RoomJoin`의 `RoomState` 응답은 **요청한 본인에게만**
갔음. bob이 들어와도 alice는 아무 통보를 못 받아서, 직접 `RoomList`를 다시 조회해야 멤버가
늘어난 걸 알 수 있었음. 브로드캐스트 수단이 생기자마자 고칠 수 있게 되어, 생성/참여/퇴장은 물론
**연결이 끊긴 경우까지** 방에 남은 전원에게 `RoomState`를 밀어주도록 바꿨다.
나간 본인은 이미 멤버 목록에서 빠져서 브로드캐스트 대상이 아니므로 `RoomLeaveOk`를 따로 회신한다.

**방어 코드:** 빈 문자열 거부, 500자 상한. Step 4에서 프레임 길이에 1MB 상한을 둔 것과 같은 이유로,
클라이언트가 보낸 값을 그대로 믿지 않는다. 채팅은 프레임과 달리 **여러 명에게 복제**되므로
긴 문자열 하나가 인원수만큼 증폭된다.

**테스트 클라이언트를 고쳤다:** 브로드캐스트는 요청 없이 서버가 먼저 보내는 메시지라, 기존
`Read-Framed`(올 때까지 블로킹)로는 확인이 불편했음 → 도착해 있는 것만 꺼내는 `Read-Available` 추가.
그리고 보내는 쪽과 받는 쪽이 동시에 살아있어야 해서, 창을 두 개 띄우는 대신 한 창에서 여러 연결을
만들 수 있게 `New-Client`를 추가했다. 스크립트를 불러오는 순간 연결이 하나 고정으로 생기던 것도 없앴다.

**막혔던 부분:** 빌드가 안 됐는데 원인이 코드가 아니었음. `build/vcpkg_installed` 안의 파일들이
크기·날짜는 그대로인데 **내용이 전부 NUL 바이트**로 날아가 있었음(`boost/asio.hpp` 8,386바이트가 전부 `\0`).
CMake는 `Parse error. Expected a command name, got bad character`를, 컴파일러는 include는 성공하는데
`'boost': 클래스 또는 네임스페이스 이름이 아닙니다`를 냄 — 빈 파일을 읽었으니 당연한 결과.
비정상 종료 때 쓰기 버퍼가 디스크에 안 내려가면 생기는 손상. `build/`를 지우고 재구성해서 해결.
→ 지울 때도 한 번 더 막힘: vcpkg 소스 트리에 260자를 넘는 경로가 있어서 `Remove-Item`이 실패함.
빈 폴더를 `robocopy /MIR`로 덮어씌우는 방식으로 우회.

**현재 한계 (의도적):**
- **로비 채팅 없음** — 방에 있어야만 채팅 가능. 방 밖에서 보내면 `not in a room` 에러.
- **귓속말 없음** — 특정 상대에게 보내는 건 Step 11(친구)에서.
- **`RoomState`의 members는 여전히 player_id만** — 이제 `sessions_`를 뒤지면 username을 채울 수 있지만,
  그러면 "방 정보"를 만드는 데 "연결 정보"가 섞인다. 계정 개념이 생기는 시점에 하는 게 맞다고 판단해 미룸.
- **브로드캐스트할 때마다 JSON을 인원수만큼 직렬화** — `send()`가 각자 `dump()`를 한다.
  인원이 적어서 지금은 문제 없음. 한 번 직렬화해서 프레임을 공유하는 건 필요해지면.

**검증:** alice/bob/carol 세 연결을 한 창에서 띄워서 확인.

1. 로그인 3명 → `playerId` 1, 2, 3 발급
2. alice가 `RoomCreate` → alice만 `RoomState` 수신, bob은 조용함 (방 밖에는 안 감)
3. **bob이 `RoomJoin` → bob과 alice 둘 다** `{"id":1,"members":[1,2],"name":"Boss Room"}` 수신
   — 이전 단계에서는 alice가 아무것도 못 받던 부분
4. alice가 `ChatSend` → 양쪽 모두
   `{"fromId":1,"fromName":"alice","text":"bob 왔냐"}` 수신 (보낸 본인도 포함)
5. bob이 회신 → 양쪽 모두 `{"fromId":2,"fromName":"bob","text":"ㅇㅇ 방금"}` 수신
6. 방에 없는 carol이 `ChatSend` → `{"message":"not in a room"}`, 이때 alice는 아무것도 안 받음
7. 빈 문자열 → `{"message":"text required"}` / 501자 → `{"message":"text too long"}`
8. bob이 `RoomLeave` → bob은 `RoomLeaveOk`, alice는 `members:[1]`로 줄어든 `RoomState` 수신
9. bob이 재입장 후 **소켓을 그냥 닫음** → alice가 `members:[1]` `RoomState` 자동 수신
   (요청 없이 서버가 먼저 밀어준 것)
10. carol이 `RoomList` → `[{"id":1,"members":[1],"name":"Boss Room"}]`

</details>