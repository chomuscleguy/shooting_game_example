# shooting_game_example

2D 멀티플레이 슈팅게임. 서버(C++/Boost.Asio)를 처음부터 하나씩 직접 만들어가는 프로젝트.

서버 권위형(server-authoritative) 구조로 간다 — 방 상태, 보스 체력, 경매 결과 같은
공유 상태는 항상 서버가 판정하고, 클라이언트(Unity/C#)는 요청만 보낸다.

## 진행 상황

- [x] Step 1: TCP 접속을 받아서 로그만 찍는 서버
- [x] Step 2: 받은 데이터를 그대로 돌려주는 echo 서버
- [x] Step 3: 여러 명이 동시에 접속 가능하게 (async_accept + io_context)
- [ ] Step 4: 길이-prefix + JSON 프로토콜 얹기
- [ ] Step 5: 로그인 (유저네임)
- [ ] Step 6: 방 생성/참여/퇴장
- [ ] Step 7: 채팅 (방 단위 브로드캐스트)
- [ ] Step 8: 매칭 큐 + 보스 스폰
- [ ] Step 9: 보스 공격 + 클리어 리워드
- [ ] Step 10: 경매 (등록/입찰/타이머 마감)
- [ ] Step 11: 친구 (요청/수락/목록)
- [ ] Step 12: 라즈베리파이 배포 (systemd 서비스로 상시 구동)

## 빌드

Windows + Visual Studio + vcpkg:
1. Visual Studio에서 이 폴더를 "폴더 열기"로 열기
2. 구성(Configuration) 드롭다운에서 `default` 선택 (vcpkg로 boost-asio 자동 설치됨)
3. `Ctrl+Shift+B`로 빌드, `Ctrl+F5`로 실행

## Devlog

### Step 1 — 접속만 받아서 로그 찍기

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

### Step 2 — echo 서버

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

### Step 3 — 비동기로 전환 (여러 명 동시 접속)

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