#include <iostream>
#include <memory>
#include <string>
#include <cstdint>
#include <deque>
#include <vector>
#include <unordered_map>
#include <algorithm>
#include <chrono>
#include <boost/asio.hpp>
#include <nlohmann/json.hpp>

using boost::asio::ip::tcp;

// ============================================================
// Item
// ============================================================
// 개체마다 고유 번호를 준다. 같은 이름의 물건이 둘이어도 서로 다른 개체라,
// 경매에 걸린 그 물건을 지목하고 추적할 수 있어야 하기 때문.
struct Item {
    uint64_t id = 0;
    std::string name;

    nlohmann::json to_json() const {
        return nlohmann::json{ {"id", id}, {"name", name} };
    }
};

// 보스가 떨구는 아이템. 종류가 늘면 데이터 파일로 뺄 자리.
inline constexpr const char* kBossDropName = "Slime Core";

// ============================================================
// Boss
// ============================================================
struct Boss {
    std::string name;
    int32_t max_hp = 0;
    int32_t hp = 0;
};

struct BossTemplate {
    const char* name;
    int32_t max_hp;
    uint64_t clear_reward;      // 클리어 시 방 전체가 나눠 갖는 금액
};
inline constexpr BossTemplate kDefaultBoss{ "Slime King", 500, 1000 };

// 공격 한 번의 데미지는 서버가 정한다. 클라이언트는 "때렸다"만 보낸다.
inline constexpr int32_t kAttackDamage = 50;

// ============================================================
// Room
// ============================================================
enum class RoomPhase { Waiting, BossFight, Cleared };

inline const char* to_string(RoomPhase phase) {
    switch (phase) {
    case RoomPhase::BossFight: return "boss_fight";
    case RoomPhase::Cleared:   return "cleared";
    default:                   return "waiting";
    }
}

// 파티 경매 — 보스 드랍을 파티원끼리 나눠 갖기 위한 것.
// 낙찰자가 아이템을 갖고, 낸 돈은 나머지 파티원이 나눠 갖는다.
inline constexpr int kAuctionSeconds = 30;

// 전투 중 끊긴 사람의 자리를 얼마나 지켜줄지. 지나면 방에서 정리한다.
inline constexpr int kReconnectGraceSeconds = 60;

struct LootAuction {
    bool active = false;
    Item item;
    uint64_t highest_bid = 0;
    uint64_t highest_bidder = 0;        // 0 = 아직 아무도 안 걸었음
    std::chrono::steady_clock::time_point ends_at;

    int seconds_left() const {
        auto now = std::chrono::steady_clock::now();
        if (now >= ends_at) return 0;
        return static_cast<int>(
            std::chrono::duration_cast<std::chrono::seconds>(ends_at - now).count());
    }

    nlohmann::json to_json() const {
        nlohmann::json j;
        j["item"] = item.to_json();
        j["highestBid"] = highest_bid;
        j["highestBidder"] = highest_bidder;
        j["secondsLeft"] = seconds_left();
        return j;
    }
};


struct Room {
    uint32_t id = 0;
    std::string name;
    std::vector<uint64_t> members;   // 참여자들의 player_id
    RoomPhase phase = RoomPhase::Waiting;      //bool in_battle 대신
    Boss boss;
    LootAuction loot;

    bool has_boss() const { return phase != RoomPhase::Waiting; }

    // 드랍된 아이템으로 경매를 연다. 마감 시각을 지금 계산해 박아둔다.
    void start_loot_auction(const Item& drop) {
        loot = LootAuction{};           // 이전 경매 흔적 초기화
        loot.active = true;
        loot.item = drop;
        loot.ends_at = std::chrono::steady_clock::now()
            + std::chrono::seconds(kAuctionSeconds);
    }

    // 입찰을 받는다. 성공하면 밀려난 이전 최고 입찰자를 out으로 알려준다
    // (0이면 없음). 환불은 호출하는 쪽이 한다 — Room은 재화를 모른다.
    bool place_bid(uint64_t bidder, uint64_t amount, uint64_t& outbid_player,
        uint64_t& outbid_amount) {
        if (!loot.active) return false;
        if (amount <= loot.highest_bid) return false;

        outbid_player = loot.highest_bidder;
        outbid_amount = loot.highest_bid;

        loot.highest_bidder = bidder;
        loot.highest_bid = amount;
        return true;
    }

    bool auction_expired() const {
        return loot.active && std::chrono::steady_clock::now() >= loot.ends_at;
    }

    // 경매를 닫고 결과를 남긴다. Room은 재화를 모르므로 정산은 밖에서 한다.
    void close_loot_auction() {
        loot.active = false;
    }

    void spawn_boss(const BossTemplate& t) {
        boss.name = t.name;
        boss.max_hp = t.max_hp;
        boss.hp = t.max_hp;
        phase = RoomPhase::BossFight;          
    }

    // 데미지를 적용하고, 이번 공격으로 쓰러졌으면 true.
    // 얼마나 깎을지는 호출하는 쪽이 아니라 서버가 정한 값이 넘어온다.
    bool attack_boss(int32_t damage) {
        if (phase != RoomPhase::BossFight) return false;

        boss.hp = std::max(0, boss.hp - damage);
        if (boss.hp == 0) {
            phase = RoomPhase::Cleared;
            return true;
        }
        return false;
    }

    nlohmann::json to_json() const {
        nlohmann::json j;
        j["id"] = id;
        j["name"] = name;
        j["members"] = members;
        j["state"] = to_string(phase);         
        // 보스가 없는 방에 빈 보스를 실어 보내면 클라이언트가 그걸 또 걸러야 한다.
        // state로 먼저 구분하고, 있을 때만 싣는다.
        if (has_boss()) {                      // in_battle 대신
            j["boss"] = {
                { "name", boss.name },
                { "maxHp", boss.max_hp },
                { "hp", boss.hp },
            };
        }

        if (loot.active) {
            j["loot"] = loot.to_json();
        }

        return j;
    }
};

// ============================================================
// PlayerRegistry — 계정 정보. 연결이 끊겨도 살아남는다.
// ============================================================
// 지금까지 플레이어 정보(번호, 유저네임)는 Session 안에 있었고 연결이 끊기면
// 같이 사라졌다. 보상으로 받은 재화는 그러면 안 되므로, 연결보다 오래 사는
// 정보를 담을 자리가 Session 바깥에 필요해졌다.
class PlayerRegistry {
public:
    struct Player {
        uint64_t id = 0;
        std::string username;
        uint64_t currency = 0;
        bool online = false;
        std::vector<Item> inventory;
    };

    // 처음 보는 이름이면 계정을 만들고, 아는 이름이면 그 계정을 그대로 돌려준다.
    // 비밀번호는 여전히 없다 — 이름만 대면 그 계정이 된다(Step 5의 한계 그대로).
    Player& login(const std::string& username) {
        auto it = name_to_id_.find(username);
        if (it != name_to_id_.end()) {
            return players_[it->second];
        }

        uint64_t id = next_id_++;
        Player& p = players_[id];
        p.id = id;
        p.username = username;
        name_to_id_[username] = id;
        return p;
    }

    Player* find(uint64_t player_id) {
        auto it = players_.find(player_id);
        return (it == players_.end()) ? nullptr : &it->second;
    }

    void grant_currency(uint64_t player_id, uint64_t amount) {
        if (Player* p = find(player_id)) p->currency += amount;
    }

    // 새 개체를 만든다. 아직 누구의 것도 아니다 — 경매에 걸릴 물건.
    Item make_item(const std::string& name) {
        return Item{ next_item_id_++, name };
    }

    // 이미 존재하는 개체를 인벤토리에 넣는다. 번호가 유지된다.
    void give_item(uint64_t player_id, const Item& item) {
        if (Player* p = find(player_id)) p->inventory.push_back(item);
    }

    // 재화를 깎는다. 잔액이 모자라면 아무 일도 안 하고 false.
    bool spend_currency(uint64_t player_id, uint64_t amount) {
        Player* p = find(player_id);
        if (!p || p->currency < amount) return false;

        p->currency -= amount;
        return true;
    }

    void set_online(uint64_t player_id, bool online) {
        if (Player* p = find(player_id)) p->online = online;
    }

private:
    std::unordered_map<uint64_t, Player> players_;
    std::unordered_map<std::string, uint64_t> name_to_id_;
    uint64_t next_id_ = 1;
    uint64_t next_item_id_ = 1;
};

class Server;   // 전방 선언 — Session이 Server&를 갖기 위해 필요

// ============================================================
// Session — 연결 하나를 담당
// ============================================================
class Session : public std::enable_shared_from_this<Session> {
public:
    Session(tcp::socket socket, Server& server)
        : socket_(std::move(socket)), server_(server) {
    }

    void start() {
        do_read_header();
    }

    void send(const nlohmann::json& message) {
        std::string body = message.dump();
        uint32_t len = static_cast<uint32_t>(body.size());

        std::string frame;
        frame.reserve(4 + body.size());
        frame.push_back(static_cast<char>((len >> 24) & 0xFF));
        frame.push_back(static_cast<char>((len >> 16) & 0xFF));
        frame.push_back(static_cast<char>((len >> 8) & 0xFF));
        frame.push_back(static_cast<char>(len & 0xFF));
        frame += body;

        bool write_in_progress = !write_queue_.empty();
        write_queue_.push_back(std::move(frame));
        if (!write_in_progress) {
            do_write();
        }
    }

    void close() {
        boost::system::error_code ec;
        socket_.shutdown(tcp::socket::shutdown_both, ec);
        socket_.close(ec);
    }

private:
    // ---- 네트워크 ----
    void do_write() {
        std::shared_ptr<Session> self(shared_from_this());
        boost::asio::async_write(socket_, boost::asio::buffer(write_queue_.front()),
            [this, self](boost::system::error_code ec, std::size_t /*length*/) {
                if (ec) {
                    return;
                }
                write_queue_.pop_front();
                if (!write_queue_.empty()) {
                    do_write();
                }
            });
    }

    void do_read_header() {
        std::shared_ptr<Session> self(shared_from_this());
        boost::asio::async_read(socket_, boost::asio::buffer(header_, 4),
            [this, self](boost::system::error_code ec, std::size_t /*length*/) {
                if (ec) {
                    if (ec == boost::asio::error::eof) {
                        std::cout << "Client disconnected" << '\n';
                    }
                    on_disconnect();
                    return;
                }

                uint32_t body_length =
                    (static_cast<uint32_t>(header_[0]) << 24) |
                    (static_cast<uint32_t>(header_[1]) << 16) |
                    (static_cast<uint32_t>(header_[2]) << 8) |
                    (static_cast<uint32_t>(header_[3]));

                if (body_length == 0 || body_length > 1024 * 1024) {
                    std::cout << "Invalid body length: " << body_length << '\n';
                    on_disconnect();
                    return;
                }
                do_read_body(body_length);
            });
    }

    void do_read_body(uint32_t body_length) {
        body_.resize(body_length);
        std::shared_ptr<Session> self(shared_from_this());
        boost::asio::async_read(socket_, boost::asio::buffer(body_.data(), body_length),
            [this, self](boost::system::error_code ec, std::size_t /*length*/) {
                if (ec) {
                    if (ec == boost::asio::error::eof) {
                        std::cout << "Client disconnected" << '\n';
                    }
                    on_disconnect();
                    return;
                }

                handle_message(body_);
                do_read_header();
            });
    }

    // ---- 메시지 라우팅 ----
    void handle_message(const std::string& raw) {
        try {
            nlohmann::json msg = nlohmann::json::parse(raw);

            std::string type = msg.value("type", "");
            nlohmann::json data = msg.value("data", nlohmann::json::object());

            // Login은 로그인 전에도 허용
            if (type == "Login") {
                handle_login(data);
                return;
            }

            // 그 외 모든 메시지는 로그인 필수
            if (!logged_in_) {
                send_error("login required");
                return;
            }

            if (type == "Ping") {
                send({ {"type", "Pong"}, {"data", nlohmann::json::object()} });
                return;
            }

            if (type == "RoomCreate") { handle_room_create(data); return; }
            if (type == "RoomJoin") { handle_room_join(data);   return; }
            if (type == "RoomLeave") { handle_room_leave();      return; }
            if (type == "RoomList") { handle_room_list();       return; }
            if (type == "ChatSend") { handle_chat_send(data);   return; }
            if (type == "ChatSend") { handle_chat_send(data);   return; }
            if (type == "MatchEnqueue") { handle_match_enqueue(); return; } 
            if (type == "MatchCancel") { handle_match_cancel();  return; } 
            if (type == "BossAttack") { handle_boss_attack();    return; }
            if (type == "LootBid") { handle_loot_bid(data);      return; }
            if (type == "InventoryList") { handle_inventory_list(); return; }

            send_error("unknown message type: " + type);
        }
        catch (const std::exception& e) {
            std::cerr << "JSON parse error: " << e.what() << '\n';
        }
    }

    void send_error(const std::string& message) {
        nlohmann::json err;
        err["message"] = message;
        send({ {"type", "Error"}, {"data", err} });
    }

    // ---- Server를 사용하는 함수들: 선언만, 정의는 Server 뒤에 ----
    void handle_login(const nlohmann::json& data);
    void handle_room_create(const nlohmann::json& data);
    void handle_room_join(const nlohmann::json& data);
    void handle_room_leave();
    void handle_room_list();
    void handle_chat_send(const nlohmann::json& data);
    void handle_match_enqueue();      
    void handle_match_cancel();      
    void handle_boss_attack();
    void handle_loot_bid(const nlohmann::json& data);
    void handle_inventory_list();
    void on_disconnect();

    // ---- 멤버 ----
    tcp::socket socket_;
    unsigned char header_[4];
    std::string body_;
    std::deque<std::string> write_queue_;

    uint64_t player_id_ = 0;
    std::string username_;
    bool logged_in_ = false;

    // 방 소속(room_id_)은 Server가 들고 있다. 매칭처럼 "남을 방에 넣는" 동작이 생기면
    // 각 Session이 자기 소속을 따로 들고 있는 구조로는 갱신할 방법이 없다.

    Server& server_;
};

// ============================================================
// Server — 공유 상태(방 목록)와 접속 수락을 담당
// ============================================================
class Server {
public:
    Server(boost::asio::io_context& io, unsigned short port)
        : acceptor_(io, tcp::endpoint(tcp::v4(), port)), tick_timer_(io) {
    }

    void start() {
        std::cout << "Listening on port "
            << acceptor_.local_endpoint().port() << "...\n";
        do_accept();
        start_tick_timer();
    }

    // ---- 방 관리 ----
    // "누가 어느 방에 있나"는 rooms_[].members에도 들어있지만, 번호로 거꾸로 찾으려면
    // 방 전체를 뒤져야 한다. 역방향 색인을 따로 둬서 O(1)로 찾는다.
    // 같은 사실이 두 군데 있으므로, 넣고 빼는 건 반드시 이 클래스 안에서만 한다.
    uint32_t room_of(uint64_t player_id) const {
        auto it = player_to_room_.find(player_id);
        return (it == player_to_room_.end()) ? 0 : it->second;   // 0 = 어느 방에도 없음
    }

    uint32_t create_room(const std::string& name, uint64_t player_id) {
        uint32_t id = next_room_id_++;
        Room room;
        room.id = id;
        room.name = name;
        room.members.push_back(player_id);
        rooms_.emplace(id, std::move(room));
        player_to_room_[player_id] = id;
        return id;
    }

    Room* find_room(uint32_t room_id) {
        auto it = rooms_.find(room_id);
        return (it == rooms_.end()) ? nullptr : &it->second;
    }

    bool join_room(uint32_t room_id, uint64_t player_id) {
        Room* room = find_room(room_id);
        if (!room) return false;

        auto& m = room->members;
        if (std::find(m.begin(), m.end(), player_id) != m.end()) return false;

        m.push_back(player_id);
        player_to_room_[player_id] = room_id;
        return true;
    }

    // 어느 방에 있었는지를 돌려준다(없었으면 0). 호출한 쪽이 그 방에 브로드캐스트할 수 있게.
    uint32_t leave_current_room(uint64_t player_id) {
        uint32_t room_id = room_of(player_id);
        if (room_id == 0) return 0;

        player_to_room_.erase(player_id);

        Room* room = find_room(room_id);
        if (room) {
            auto& m = room->members;
            m.erase(std::remove(m.begin(), m.end(), player_id), m.end());
            if (m.empty()) {
                rooms_.erase(room_id);   // 아무도 없는 방은 삭제
            }
        }
        return room_id;
    }

    nlohmann::json room_list_json() const {
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& entry : rooms_) {
            arr.push_back(entry.second.to_json());
        }
        return arr;
    }

    void kick(uint64_t player_id) {
        auto it = sessions_.find(player_id);
        if (it == sessions_.end()) return;

        it->second->close();
    }

    // ---- 세션 레지스트리 ----
    // 방은 멤버를 player_id로만 들고 있어서, 그 번호만으로는 메시지를 보낼 수 없다.
    // "번호 -> 그 사람의 연결"을 이어주는 표가 여기 필요해짐.
    void register_session(uint64_t player_id, std::shared_ptr<Session> session) {
        sessions_[player_id] = std::move(session);
    }

    // 쫓겨난 세션이 뒤늦게 정리에 들어올 수 있다. 그 사이 같은 번호로 새 연결이
    // 등록됐다면, 지금 맵에 있는 건 남의 것이므로 건드리면 안 된다.
    void unregister_session(uint64_t player_id, const Session* who) {
        auto it = sessions_.find(player_id);
        if (it == sessions_.end()) return;
        if (it->second.get() != who) return;   // 이미 새 세션이 자리를 차지함

        sessions_.erase(it);
    }

    // ---- 브로드캐스트 ----
    void broadcast_to_room(uint32_t room_id, const nlohmann::json& message) {
        Room* room = find_room(room_id);
        if (!room) return;

        for (uint64_t member_id : room->members) {
            auto it = sessions_.find(member_id);
            if (it != sessions_.end()) {
                it->second->send(message);
            }
        }
    }

    // 방이 이미 삭제됐으면(마지막 사람이 나감) 아무에게도 안 보냄
    void broadcast_room_state(uint32_t room_id) {
        Room* room = find_room(room_id);
        if (!room) return;

        broadcast_to_room(room_id,
            { {"type", "RoomState"}, {"data", room->to_json()} });
    }

    // ---- 전투 ----
    // 데미지는 서버가 정한다(kAttackDamage). 호출하는 쪽은 "누가 때렸다"만 전달.
    void attack_boss(uint32_t room_id, uint64_t attacker_id,
        const std::string& attacker_name) {
        Room* room = find_room(room_id);
        if (!room) return;

        bool defeated = room->attack_boss(kAttackDamage);

        nlohmann::json d;
        d["hp"] = room->boss.hp;
        d["maxHp"] = room->boss.max_hp;
        d["damage"] = kAttackDamage;
        d["attackerId"] = attacker_id;
        d["attackerName"] = attacker_name;
        broadcast_to_room(room_id, { {"type", "BossState"}, {"data", d} });

        if (defeated) {
            std::cout << "Boss cleared in room " << room_id << std::endl;
            distribute_clear_reward(*room);

            // 드랍 아이템으로 파티 경매 시작
            Item drop = players_.make_item(kBossDropName);
            room->start_loot_auction(drop);

            broadcast_room_state(room_id);   // state:"cleared" + loot 포함
            broadcast_to_room(room_id,
                { {"type", "LootAuctionStarted"}, {"data", room->loot.to_json()} });
        }
    }

    // 입찰: 돈을 먼저 잡아두고(에스크로), 밀려난 사람에게는 돌려준다.
   // 실패 사유는 호출한 Session이 판정해서 회신한다.
    bool bid_on_loot(uint32_t room_id, uint64_t bidder, uint64_t amount) {
        Room* room = find_room(room_id);
        if (!room || !room->loot.active) return false;
        if (amount <= room->loot.highest_bid) return false;

        // 돈부터 잡아둔다. 모자라면 여기서 실패.
        if (!players_.spend_currency(bidder, amount)) return false;

        uint64_t outbid_player = 0;
        uint64_t outbid_amount = 0;
        if (!room->place_bid(bidder, amount, outbid_player, outbid_amount)) {
            players_.grant_currency(bidder, amount);   // 되돌리기
            return false;
        }

        // 밀려난 사람에게 환불
        if (outbid_player != 0) {
            players_.grant_currency(outbid_player, outbid_amount);
            auto it = sessions_.find(outbid_player);
            if (it != sessions_.end()) {
                PlayerRegistry::Player* p = players_.find(outbid_player);
                nlohmann::json d;
                d["currency"] = outbid_amount;
                d["reason"] = "outbid";
                d["balance"] = p ? p->currency : 0;
                it->second->send({ {"type", "RewardGrant"}, {"data", d} });
            }
        }

        broadcast_to_room(room_id,
            { {"type", "LootBidUpdate"}, {"data", room->loot.to_json()} });
        return true;
    }

    // 마감 처리: 낙찰이면 아이템을 주고 낙찰금을 나머지 파티원이 나눠 갖는다.
    // 아무도 안 걸었으면 아이템은 소멸한다.
    void close_auction(Room& room) {
        LootAuction result = room.loot;     // 닫기 전에 결과를 복사해둔다
        room.close_loot_auction();

        nlohmann::json d;
        d["item"] = result.item.to_json();

        if (result.highest_bidder == 0) {
            d["winnerId"] = 0;
            d["winningBid"] = 0;
            d["result"] = "expired";        // 유찰 — 아이템 소멸
            broadcast_to_room(room.id,
                { {"type", "LootAuctionClosed"}, {"data", d} });
            return;
        }

        // 낙찰자에게 아이템
        players_.give_item(result.highest_bidder, result.item);

        // 낙찰금은 낙찰자를 뺀 나머지가 균등 분배 (돈은 입찰 때 이미 잡아뒀다)
        std::vector<uint64_t> others;
        for (uint64_t pid : room.members) {
            if (pid != result.highest_bidder) others.push_back(pid);
        }

        if (!others.empty()) {
            uint64_t share = result.highest_bid / others.size();
            for (uint64_t pid : others) {
                players_.grant_currency(pid, share);

                auto it = sessions_.find(pid);
                if (it == sessions_.end()) continue;

                PlayerRegistry::Player* p = players_.find(pid);
                nlohmann::json r;
                r["currency"] = share;
                r["reason"] = "loot_share";
                r["balance"] = p ? p->currency : 0;
                it->second->send({ {"type", "RewardGrant"}, {"data", r} });
            }
        }

        d["winnerId"] = result.highest_bidder;
        d["winningBid"] = result.highest_bid;
        d["result"] = "sold";
        broadcast_to_room(room.id,
            { {"type", "LootAuctionClosed"}, {"data", d} });
    }

    // 전투 중 끊김 — 자리를 지켜주되 마감 시각을 박아둔다.
    void hold_seat(uint64_t player_id) {
        pending_reconnect_[player_id] = std::chrono::steady_clock::now()
            + std::chrono::seconds(kReconnectGraceSeconds);
    }

    void clear_seat_hold(uint64_t player_id) {
        pending_reconnect_.erase(player_id);
    }

    // 클리어 보상을 방 인원수로 균등 분배한다. 나머지는 버린다.
    void distribute_clear_reward(const Room& room) {
        if (room.members.empty()) return;

        uint64_t share = kDefaultBoss.clear_reward / room.members.size();

        for (uint64_t pid : room.members) {
            players_.grant_currency(pid, share);

            auto it = sessions_.find(pid);
            if (it == sessions_.end()) continue;   // 끊긴 사람은 알림만 생략

            PlayerRegistry::Player* p = players_.find(pid);

            nlohmann::json d;
            d["currency"] = share;
            d["reason"] = "boss_clear";
            d["balance"] = p ? p->currency : 0;
            it->second->send({ {"type", "RewardGrant"}, {"data", d} });
        }
    }

    // ---- 매칭 큐 ----
    static constexpr std::size_t kPartySize = 2;

    bool in_queue(uint64_t player_id) const {
        return std::find(match_queue_.begin(), match_queue_.end(), player_id)
            != match_queue_.end();
    }

    std::size_t queue_size() const { return match_queue_.size(); }

    void cancel_match(uint64_t player_id) {
        match_queue_.erase(
            std::remove(match_queue_.begin(), match_queue_.end(), player_id),
            match_queue_.end());
    }

    // ---- 계정 ----
    PlayerRegistry& players() { return players_; }

    // 매칭이 성사되면 방을 만들어 전원을 넣고 MatchFound까지 보낸 뒤 true.
    // 이 함수만 "남의 방 소속을 바꾸는" 일을 한다 — Session은 자기 것밖에 못 바꾼다.
    bool enqueue_for_match(uint64_t player_id) {
        match_queue_.push_back(player_id);
        if (match_queue_.size() < kPartySize) {
            return false;
        }

        std::vector<uint64_t> party(match_queue_.begin(),
            match_queue_.begin() + kPartySize);
        match_queue_.erase(match_queue_.begin(), match_queue_.begin() + kPartySize);

        uint32_t id = next_room_id_++;
        Room room;
        room.id = id;
        room.name = "Matchmade Room " + std::to_string(id);
        room.members = party;
        room.spawn_boss(kDefaultBoss);
        rooms_.emplace(id, std::move(room));

        for (uint64_t pid : party) {
            player_to_room_[pid] = id;
        }

        std::cout << "Match found -> room " << id << " (" << party.size() << " players)\n";

        broadcast_to_room(id,
            { {"type", "MatchFound"}, {"data", find_room(id)->to_json()} });
        return true;
    }

private:
    void do_accept() {
        acceptor_.async_accept(
            [this](boost::system::error_code ec, tcp::socket socket) {
                if (!ec) {
                    std::cout << "Client connected: "
                        << socket.remote_endpoint() << '\n';
                    std::make_shared<Session>(
                        std::move(socket), *this)->start();
                }
                do_accept();
            });
    }

    void start_tick_timer() {
        tick_timer_.expires_after(std::chrono::seconds(1));
        tick_timer_.async_wait([this](boost::system::error_code ec) {
            if (ec) return;        // 타이머가 취소됨 (서버 종료 등)
            on_tick();
            start_tick_timer();    // 다음 틱 예약
            });
    }

    void on_tick() {
        // 마감된 경매를 찾아 정산한다. 여기가 "아무도 안 보냈는데 서버가 움직이는" 곳.
        for (auto& entry : rooms_) {
            if (entry.second.auction_expired()) {
                close_auction(entry.second);
            }
        }

        // 돌아오지 않은 사람의 자리를 정리한다.
        // 순회 중에 지울 수 없으니 먼저 모아두고 그 다음에 처리한다.
        auto now = std::chrono::steady_clock::now();
        std::vector<uint64_t> expired;
        for (const auto& entry : pending_reconnect_) {
            if (now >= entry.second) expired.push_back(entry.first);
        }

        for (uint64_t pid : expired) {
            pending_reconnect_.erase(pid);

            uint32_t left_room = leave_current_room(pid);
            if (left_room != 0) {
                std::cout << "Reconnect timeout: player " << pid
                    << " removed from room " << left_room << std::endl;
                broadcast_room_state(left_room);
            }
        }
    }

    tcp::acceptor acceptor_;
    boost::asio::steady_timer tick_timer_;
    uint32_t next_room_id_ = 1;
    std::unordered_map<uint32_t, Room> rooms_;
    std::unordered_map<uint64_t, uint32_t> player_to_room_;
    std::unordered_map<uint64_t, std::shared_ptr<Session>> sessions_;  // 로그인한 연결만
    std::vector<uint64_t> match_queue_;
    // 전투 중 끊겨서 자리를 비워둔 사람들. 값은 "이때까지 안 오면 정리" 시각.
    std::unordered_map<uint64_t, std::chrono::steady_clock::time_point> pending_reconnect_;
    PlayerRegistry players_;
};

// ============================================================
// Session의 미뤄둔 정의들 (Server의 내용을 알아야 하므로 여기에)
// ============================================================
void Session::handle_login(const nlohmann::json& data) {
    if (logged_in_) {
        send_error("already logged in");
        return;
    }

    std::string username = data.value("username", "");
    if (username.empty()) {
        send_error("username required");
        return;
    }

    // 처음 보는 이름이면 계정이 새로 생기고, 아는 이름이면 그 계정으로 들어간다.
    PlayerRegistry::Player& p = server_.players().login(username);

    // 같은 계정이 이미 접속 중이면 기존 연결을 밀어낸다.
    if (p.online) {
        std::cout << "Kicking previous session of " << username << '\n';
        server_.kick(p.id);
    }
    p.online = true;

    player_id_ = p.id;
    username_ = p.username;
    logged_in_ = true;

    server_.register_session(player_id_, shared_from_this());

    std::cout << "Login: " << username_ << " (id=" << player_id_ << ")\n";

    nlohmann::json ok;
    ok["playerId"] = player_id_;
    ok["username"] = username_;
    ok["currency"] = p.currency;
    send({ {"type", "LoginOk"}, {"data", ok} });

    //자리를 비워둔 방이 있으면 현재 상태를 보내준다
    uint32_t room_id = server_.room_of(player_id_);
    if (room_id != 0) {
        Room* room = server_.find_room(room_id);
        if (room) {
            server_.clear_seat_hold(player_id_);
            std::cout << username_ << " reconnected to room " << room_id << '\n';
            send({ {"type", "RoomState"}, {"data", room->to_json()} });
        }
    }
}

void Session::handle_room_create(const nlohmann::json& data) {
    if (server_.room_of(player_id_) != 0) {
        send_error("already in a room");
        return;
    }
    std::string name = data.value("name", "");
    if (name.empty()) {
        send_error("room name required");
        return;
    }

    uint32_t id = server_.create_room(name, player_id_);
    std::cout << username_ << " created room " << id << " (" << name << ")\n";

    server_.broadcast_room_state(id);
}

void Session::handle_room_join(const nlohmann::json& data) {
    if (server_.room_of(player_id_) != 0) {
        send_error("already in a room");
        return;
    }
    uint32_t room_id = data.value("roomId", 0u);
    if (!server_.join_room(room_id, player_id_)) {
        send_error("cannot join room " + std::to_string(room_id));
        return;
    }

    std::cout << username_ << " joined room " << room_id << '\n';

    // 들어온 사람만이 아니라 원래 있던 사람들도 새 멤버 목록을 받는다
    server_.broadcast_room_state(room_id);
}

void Session::handle_room_leave() {
    uint32_t left_room = server_.leave_current_room(player_id_);
    if (left_room == 0) {
        send_error("not in a room");
        return;
    }
    std::cout << username_ << " left room " << left_room << '\n';

    // 나간 본인은 이미 방 멤버가 아니라 브로드캐스트 대상이 아님 -> 따로 회신
    send({ {"type", "RoomLeaveOk"}, {"data", nlohmann::json::object()} });
    server_.broadcast_room_state(left_room);
}

void Session::handle_room_list() {
    send({ {"type", "RoomListResult"}, {"data", server_.room_list_json()} });
}

void Session::handle_chat_send(const nlohmann::json& data) {
    uint32_t room_id = server_.room_of(player_id_);  
    if (room_id == 0) {                              
        send_error("not in a room");
        return;
    }

    std::string text = data.value("text", "");
    if (text.empty()) {
        send_error("text required");
        return;
    }
    if (text.size() > 500) {
        send_error("text too long");
        return;
    }

    // 보내는 사람이 누군지는 자기 Session이 이미 알고 있다.
    // sessions_가 필요한 건 "누가 보냈나"가 아니라 "누구에게 전달하나" 쪽.
    nlohmann::json chat;
    chat["fromId"] = player_id_;
    chat["fromName"] = username_;
    chat["text"] = text;

    server_.broadcast_to_room(room_id,
        { {"type", "ChatBroadcast"}, {"data", chat} });
}

void Session::handle_boss_attack() {
    uint32_t room_id = server_.room_of(player_id_);
    if (room_id == 0) {
        send_error("not in a room");
        return;
    }

    Room* room = server_.find_room(room_id);
    if (!room) {
        send_error("room not found");
        return;
    }
    if (room->phase == RoomPhase::Waiting) {
        send_error("no boss in this room");
        return;
    }
    if (room->phase == RoomPhase::Cleared) {
        send_error("boss already cleared");
        return;
    }

    server_.attack_boss(room_id, player_id_, username_);
}

void Session::handle_loot_bid(const nlohmann::json& data) {
    uint32_t room_id = server_.room_of(player_id_);
    if (room_id == 0) {
        send_error("not in a room");
        return;
    }

    Room* room = server_.find_room(room_id);
    if (!room || !room->loot.active) {
        send_error("no auction running");
        return;
    }

    uint64_t amount = data.value("amount", 0ull);
    if (amount == 0) {
        send_error("amount required");
        return;
    }
    if (amount <= room->loot.highest_bid) {
        send_error("bid too low");
        return;
    }

    PlayerRegistry::Player* me = server_.players().find(player_id_);
    if (!me || me->currency < amount) {
        send_error("not enough currency");
        return;
    }

    if (!server_.bid_on_loot(room_id, player_id_, amount)) {
        send_error("bid rejected");
        return;
    }
}

void Session::handle_inventory_list() {
    PlayerRegistry::Player* p = server_.players().find(player_id_);
    if (!p) {
        send_error("player not found");
        return;
    }

    nlohmann::json items = nlohmann::json::array();
    for (const Item& item : p->inventory) {
        items.push_back(item.to_json());
    }

    nlohmann::json d;
    d["items"] = items;
    d["currency"] = p->currency;
    send({ {"type", "InventoryResult"}, {"data", d} });
}


void Session::handle_match_enqueue() {
    if (server_.room_of(player_id_) != 0) {
        send_error("already in a room");
        return;
    }
    if (server_.in_queue(player_id_)) {
        send_error("already in queue");
        return;
    }

    // 성사되면 MatchFound가 이미 나갔으므로 여기서 더 보낼 게 없다.
    if (server_.enqueue_for_match(player_id_)) {
        return;
    }

    std::cout << username_ << " queued for match\n";

    nlohmann::json d;
    d["waiting"] = server_.queue_size();
    d["needed"] = Server::kPartySize;
    send({ {"type", "MatchQueued"}, {"data", d} });
}

void Session::handle_match_cancel() {
    if (!server_.in_queue(player_id_)) {
        send_error("not in queue");
        return;
    }
    server_.cancel_match(player_id_);
    std::cout << username_ << " cancelled matchmaking\n";

    send({ {"type", "MatchCancelOk"}, {"data", nlohmann::json::object()} });
}

void Session::on_disconnect() {
    server_.cancel_match(player_id_);

    uint32_t room_id = server_.room_of(player_id_);
    Room* room = server_.find_room(room_id);

    // 전투 중이면 자리를 비워둔다 — 같은 계정으로 돌아오면 그대로 복귀
    bool keep_seat = (room && room->phase == RoomPhase::BossFight);

    if (!keep_seat) {
        uint32_t left_room = server_.leave_current_room(player_id_);
        if (left_room != 0) {
            server_.broadcast_room_state(left_room);
        }
    }
    else {
        server_.hold_seat(player_id_); 
    }

    if (logged_in_) {
        server_.players().set_online(player_id_, false);
        server_.unregister_session(player_id_, this);
        logged_in_ = false;
    }
}

// ============================================================
int main() {
    try {
        boost::asio::io_context io;
        Server server(io, 7777);
        server.start();
        io.run();
    }
    catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << '\n';
    }
}