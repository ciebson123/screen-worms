#include <iostream>
#include <cstdint>
#include <sys/socket.h>
#include <unistd.h>
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <chrono>
#include <sys/time.h>
#include <netinet/in.h>
#include <vector>
#include <algorithm>
#include <cmath>
#include <mutex>
#include <condition_variable>
#include <unordered_map>
#include <utility>
#include "communication.h"
#include "err.h"
#include "crc.h"

#define RANDOM_MULT 279410273
#define RANDOM_MOD 4294967291

#define MAX_CONNECTED 25
#define MAX_IDLE_TIME 2000000

using namespace std;


/* globals */

uint8_t connected_players = 0;
uint8_t ready_players = 0;
int sock_fd = 0;

const char *options = "p:s:t:v:w:h:";

constexpr double DEG_TO_RADIANS = M_PI / 180.0;

uint64_t turning_speed = DEFAULT_TURNING_SPEED;
uint64_t rounds_per_second = DEFAULT_ROUNDS_PER_SECOND;
uint16_t port = DEFAULT_SERWER_PORT;
uint64_t maxx = DEFAULT_WIDTH;
uint64_t maxy = DEFAULT_HEIGHT;

mutex mut{};
mutex wait_for_players_mut{};
condition_variable wait_for_players{};

uint64_t current_time_in_microseconds() {
    timeval curr_time{};
    gettimeofday(&curr_time, nullptr);
    return 1000000 * curr_time.tv_sec + curr_time.tv_usec;
}

struct PlayerData {

    uint64_t last_connected, session_id;
    long double x, y;
    int32_t direction;
    bool ready_to_play, eliminated;
    uint8_t turn_direction;
    string name;

    PlayerData(uint64_t session_id, uint8_t turn_direction, const string &name) :
            last_connected(current_time_in_microseconds()), session_id(session_id),
            x(0), y(0), direction(0),
            ready_to_play(false), eliminated(false),
            turn_direction(turn_direction),
            name(name) {
        if (!name.empty())
            connected_players++;
        set_direction(turn_direction);
    }

    ~PlayerData() {
        if (ready_to_play)
            ready_players--;
        if (!name.empty())
            connected_players--;
    }

    bool operator<(const PlayerData &p2) const {
        return name < p2.name;
    }

    void set_direction(uint8_t new_turn_direction) {
        turn_direction = new_turn_direction;
        if (!ready_to_play && turn_direction != 0 && !name.empty()) {
            ready_to_play = true;
            ready_players++;
        }
    }

    void game_ended() {
        eliminated = false;
        if (ready_to_play) {
            ready_to_play = false;
            ready_players--;
        }
    }

};

struct PlayerWrapper {
    shared_ptr<PlayerData> data;

    PlayerWrapper(uint64_t session_id, uint8_t turn_direction, const string &name) :
            data(make_shared<PlayerData>(session_id, turn_direction, name)) {}

    PlayerData &operator*() {
        return *data;
    }

    bool operator<(const PlayerWrapper &p2) const {
        return *data < *p2.data;
    }

    PlayerData *operator->() {
        return data.operator->();
    }
};

struct Event {
    event_header_mess header;
    string data;
    crc32_t checksum;

    Event(const string &data, uint32_t event_no, uint8_t type) :
            header(htobe32(data.size() + EVENT_NO_TYPE_SIZE),
                   htobe32(event_no), type), data(data), checksum(calc_checksum()) {}

    crc32_t calc_checksum() {
        uint8_t mess[MAX_HOST_MESS_LEN];
        memcpy(mess, &header, EVENT_HEADER_SIZE);
        memcpy(mess + EVENT_HEADER_SIZE, data.c_str(), data.size());
        return htobe32(crc_cacl(mess, EVENT_HEADER_SIZE + data.size()));
    }

    [[nodiscard]] ssize_t size() const {
        return data.size() + EVENT_HEADER_META;
    }
};

struct GameData {
    uint32_t game_id;
    vector<PlayerWrapper> players;
    vector<Event> events;
    uint32_t active_players;
    bool board[MAX_WIDTH][MAX_HEIGHT]; // board[i][j] -> is space (i, j) eaten/being eaten

    GameData() : game_id(), players(), events(), active_players(), board() {
        for (auto &i : board)
            memset(i, false, MAX_HEIGHT);
    }

    void clear() {
        players.clear();
        events.clear();
        game_id = 0;
        active_players = 0;
        for (auto &i : board)
            memset(i, false, MAX_HEIGHT);
    }

    void end_game() {
        for (auto &p : players) {
            p->game_ended();
        }
        players.clear();
    }
} current_game{};

struct AddressBase {
    virtual ~AddressBase() = default;

    virtual sockaddr *get_address() = 0;

    virtual bool operator==(const AddressBase &a) const = 0;

    virtual socklen_t size() = 0;

    [[nodiscard]] virtual size_t hash() const = 0;
};

struct AddressIP4 : public AddressBase {
    sockaddr_in address;
    socklen_t addr_size;

    explicit AddressIP4(const sockaddr_in *address) : address(*address), addr_size(sizeof(sockaddr_in)) {
    }

    bool operator==(const AddressBase &a) const override {
        if (const auto *ptr = dynamic_cast<const AddressIP4 *>(&a))
            return address.sin_port == ptr->address.sin_port &&
                   address.sin_addr.s_addr == ptr->address.sin_addr.s_addr;
        return false;
    }

    sockaddr *get_address() override {
        return reinterpret_cast<sockaddr *>(&address);
    }

    socklen_t size() override {
        return addr_size;
    }

    [[nodiscard]] size_t hash() const override {
        return address.sin_addr.s_addr + address.sin_port;
    }
};

struct AddressIP6 : public AddressBase {
    sockaddr_in6 address;
    socklen_t addr_size;

    explicit AddressIP6(const sockaddr_in6 *address) : address(*address), addr_size(sizeof(sockaddr_in6)) {

    }

    bool operator==(const AddressBase &a) const override {
        if (const auto *ptr = dynamic_cast<const AddressIP6 *>(&a)) {
            return address.sin6_port == ptr->address.sin6_port &&
                   !memcmp(address.sin6_addr.s6_addr, ptr->address.sin6_addr.s6_addr,
                           sizeof(address.sin6_addr.s6_addr));
        }
        return false;
    }

    sockaddr *get_address() override {
        return reinterpret_cast<sockaddr *>(&address);
    }

    socklen_t size() override {
        return addr_size;
    }

    [[nodiscard]] size_t hash() const override {
        return address.sin6_addr.s6_addr[0] + address.sin6_port;
    }
};

struct AddressWrapper {
    shared_ptr<AddressBase> address;

    explicit AddressWrapper(const sockaddr_in *addr) : address(make_shared<AddressIP4>(addr)) {}

    explicit AddressWrapper(const sockaddr_in6 *addr) : address(make_shared<AddressIP6>(addr)) {}

    AddressBase &operator*() {
        return *address;
    }

    bool operator==(const AddressWrapper &a) const {
        return *address == *(a.address);
    }

    [[nodiscard]] sockaddr *get_address() const {
        return address->get_address();
    }

    [[nodiscard]] socklen_t size() const {
        return address->size();
    }

    static AddressWrapper makeAddressWrapper(const sockaddr *addr) {
        if (addr->sa_family == AF_INET)
            return AddressWrapper((sockaddr_in *) addr);
        return AddressWrapper((sockaddr_in6 *) addr);
    }

    [[nodiscard]] size_t hash() const {
        return address->hash();
    }

    AddressBase *operator->() {
        return address.operator->();
    }

};

template<>
struct std::hash<AddressWrapper> {
    std::size_t operator()(const AddressWrapper &a) const {
        return a.hash();
    }
};

unordered_map<AddressWrapper, PlayerWrapper> connections{};

/* Random */

uint64_t random_value = time(nullptr);

uint32_t rand_moodle() {
    uint32_t prev = random_value;
    uint64_t help = random_value;
    help = (help * RANDOM_MULT) % RANDOM_MOD;
    random_value = (uint32_t) help;
    return prev;
}

void parse_options(int argc, char **argv) {
    int c;
    while ((c = getopt(argc, argv, options)) != -1) {
        switch (c) {
            case 'p':
                port = strtoul(optarg, nullptr, 10);
                break;
            case 's':
                random_value = strtoul(optarg, nullptr, 10);
                break;
            case 't':
                turning_speed = strtoul(optarg, nullptr, 10);
                break;
            case 'v':
                rounds_per_second = strtoul(optarg, nullptr, 10);
                break;
            case 'w':
                maxx = strtoul(optarg, nullptr, 10);
                break;
            case 'h':
                maxy = strtoul(optarg, nullptr, 10);
                break;
            default:
                syserr("UNKNOWN OPTION");
        }
    }
}

//check if parameters are valid
void validity_check() {
    if (0 == maxx || MAX_WIDTH < maxx)
        fatal("Bad width");
    if (0 == maxy || MAX_HEIGHT < maxy)
        fatal("Bad height");
    if (0 == turning_speed || MAX_TURNING_SPEED < turning_speed)
        fatal("bad turning speed");
    if (0 == rounds_per_second || MAX_ROUND_PER_SECOND < rounds_per_second)
        fatal("bad rounds per second");
    if (0 == port)
        fatal("BAD_PORT");
    if (0 == random_value || UINT32_MAX < random_value)
        fatal("bad seed");
}


void init_socket() {
    sock_fd = socket(AF_INET6, SOCK_DGRAM, 0);
    if (sock_fd < 0)
        syserr("socket");

    sockaddr_in6 server_adress{};
    server_adress.sin6_family = AF_INET6;
    server_adress.sin6_addr = in6addr_any;
    server_adress.sin6_port = htobe16(port);
    if (bind(sock_fd, (sockaddr *) &server_adress, sizeof(server_adress)) < 0)
        syserr("bind");
}

void disconnect_old(uint64_t now) {
    auto iterator = connections.begin();
    while (iterator != connections.end()) {
        auto &player = *(iterator->second);
        if (now - player.last_connected > MAX_IDLE_TIME) {
            connections.erase(iterator++);
        } else {
            ++iterator;
        }
    }
}

bool unique_name(string &name) {
    for (auto &conn : connections) {
        auto &player = *conn.second;
        if (!player.name.empty() && player.name == name)
            return false;
    }
    return true;
}

void copy_to_buffer(uint8_t *buffer_start, Event &e) {
    uint8_t *buffer = buffer_start;
    memcpy(buffer, &e.header, EVENT_HEADER_SIZE);
    buffer += EVENT_HEADER_SIZE;
    memcpy(buffer, e.data.c_str(), e.data.size());
    buffer += e.data.size();
    memcpy(buffer, &e.checksum, sizeof(crc32_t));
}

//copys message from events to buffer
//returns (number of events copied, total lenght of copied data)
pair<ssize_t, ssize_t> make_message(uint8_t *buffer, ssize_t starting_event_no) {

    uint32_t game_id_be = htobe32(current_game.game_id);
    memcpy(buffer, &game_id_be, sizeof(uint32_t));
    ssize_t last_written = sizeof(uint32_t);
    ssize_t num_of_events = 0;

    auto it = current_game.events.begin() + starting_event_no;

    while (it != current_game.events.end()
           && last_written + it->size() < MAX_HOST_MESS_LEN) {
        copy_to_buffer(buffer + last_written, *it);
        last_written += it->size();
        ++it;
        num_of_events++;
    }
    return make_pair(num_of_events, last_written);
}

void send_to_address(const AddressWrapper &addr, uint8_t *buff, ssize_t len) {
    if (sendto(sock_fd, buff, len, MSG_DONTWAIT, addr.get_address(), addr.size()) < len) {
        if (errno == EWOULDBLOCK || errno == EAGAIN) {
            // not my problem
            return;
        } else {
            syserr("write-failure");
        }
    }
}

//bundles messages and sends them to one host
void send_events_to_one_client(const AddressWrapper &address, uint32_t next_event) {
    if (next_event >= current_game.events.size())
        return;

    uint8_t buffer[MAX_HOST_MESS_LEN];
    while (next_event < current_game.events.size()) {
        auto[event_num, len] = make_message(buffer, next_event);
        next_event += event_num;

        send_to_address(address, buffer, len);
    }
}

//bundles messages and sends them to all clients
void send_to_all_clients(size_t event_start) {
    uint8_t buffer[MAX_HOST_MESS_LEN];

    while (event_start < current_game.events.size()) {

        auto[event_num, len] = make_message(buffer, event_start);
        event_start += event_num;

        for (auto &conn : connections) {
            send_to_address(conn.first, buffer, len);
        }
    }

}

void new_client(const AddressWrapper &address, uint64_t session_id,
                uint8_t turn_direction, uint32_t next_event_no, string &name) {

    if (!unique_name(name))
        return;

    if (connections.size() == MAX_CONNECTED)
        return;

    connections.insert_or_assign(address, PlayerWrapper(session_id, turn_direction, name));

    send_events_to_one_client(address, next_event_no);
}

void send_to_known_client(const AddressWrapper &address, uint64_t session_id,
                          uint8_t turn_direction, uint32_t next_event_no, string &name) {


    auto iter = connections.find(address);
    if (iter == connections.end())
        return;

    auto &player_data = *(iter->second);

    if (session_id != player_data.session_id) {
        connections.erase(iter);

        new_client(address, session_id,
                   turn_direction, next_event_no, name);
        return;
    }

    if (name != player_data.name)
        return;

    player_data.set_direction(turn_direction);

    player_data.last_connected = current_time_in_microseconds();
    send_events_to_one_client(address, next_event_no);

}

bool valid_data(const client_to_serwer_mess &mess, uint8_t direction, size_t name_size) {
    if (name_size > MAX_PLAYER_NAME_LENGTH)
        return false;
    if (direction > 2)
        return false;
    for (size_t i = 0; i < name_size; i++) {
        if (!isgraph(mess.player_name[i]))
            return false;
    }
    return true;
}

string get_name(const char *name_buff, size_t size) {
    string result;
    result.append(name_buff, size);
    return result;
}

void process_message(const client_to_serwer_mess &message, size_t mess_size,
                     const sockaddr *client_address) {


    uint64_t session_id = be64toh(message.session_id);
    uint8_t turn_direction = message.turn_direction;

    uint32_t next_event_no = be32toh(message.next_expected_event_no);

    uint32_t name_size = mess_size - CLIENT_HEADER_SIZE;
    //reality check
    if (!valid_data(message, turn_direction, name_size))
        return;

    string name = get_name(message.player_name, name_size);

    auto address = AddressWrapper::makeAddressWrapper(client_address);

    auto iter = connections.find(address);

    if (iter == connections.end()) {
        new_client(address, session_id,
                   turn_direction, next_event_no, name);
    } else {
        send_to_known_client(address, session_id,
                             turn_direction, next_event_no, name);
    }

}

bool time_to_start() {
    return connected_players == ready_players && connected_players > 1;
}


//listens for all clients in a loop
[[noreturn]] void do_listen() {

    client_to_serwer_mess message;
    sockaddr_in6 client_address{};
    int client_size = sizeof(client_address);
    size_t mess_size;
    for (;;) {
        if ((mess_size = recvfrom(sock_fd, &message, sizeof message, 0, (sockaddr *) &client_address,
                                  (socklen_t *) &client_size)) < CLIENT_HEADER_SIZE) {
            continue;
        } else {
            lock_guard<mutex> lock(mut);
            disconnect_old(current_time_in_microseconds());
            process_message(message, mess_size, (sockaddr *) &client_address);

            if (time_to_start())
                wait_for_players.notify_all();
        }
    }
}

void add_players() {
    for (auto &conn : connections) {
        if (!conn.second->name.empty())
            current_game.players.push_back(conn.second);
    }
    sort(current_game.players.begin(), current_game.players.end());

    current_game.active_players = current_game.players.size();
}


void generate_new_game() {
    string data;
    uint32_t x_net = htobe32(maxx);
    uint32_t y_net = htobe32(maxy);
    data.append((char *) &x_net, 4);
    data.append((char *) &y_net, 4);
    for (auto &p : current_game.players) {
        //copy null bit
        data.append(p->name.c_str(), p->name.size() + 1);
    }
    current_game.events.emplace_back(data, current_game.events.size(), NEW_GAME_TYPE);
}

void generate_pixel(uint32_t x, uint32_t y, uint8_t player_num) {
    current_game.board[x][y] = true;

    string data;
    uint32_t x_net = htobe32(x);
    uint32_t y_net = htobe32(y);

    data += player_num;
    data.append((char *) &x_net, 4);
    data.append((char *) &y_net, 4);

    current_game.events.emplace_back(data, current_game.events.size(), PIXEL_TYPE);
}

void generate_player_eliminated(uint8_t player_num) {
    current_game.players[player_num]->eliminated = true;
    current_game.active_players--;

    string data;
    data += player_num;
    current_game.events.emplace_back(data, current_game.events.size(), ELIMINATED_TYPE);
}


void generate_end_game() {
    current_game.end_game();

    current_game.events.emplace_back("", current_game.events.size(), END_GAME_TYPE);
}

//check if game is still going
bool still_playing() {
    if (current_game.active_players == 1) {
        generate_end_game();
        return false;
    }
    return true;
}

//true if game has NOT ended (technically possible)
bool start_game() {
    current_game.clear();
    add_players();
    current_game.game_id = rand_moodle();
    generate_new_game();
    for (uint i = 0; i < current_game.players.size(); i++) {
        auto &player_ptr = current_game.players[i];
        auto &player = *player_ptr;
        uint32_t x = (rand_moodle() % maxx);
        uint32_t y = (rand_moodle() % maxy);
        player.x = (double) x + 0.5;
        player.y = (double) y + 0.5;
        player.direction = rand_moodle() % 360;
        if (current_game.board[x][y]) {
            generate_player_eliminated(i);
        } else {
            generate_pixel(x, y, i);
        }
    }
    bool result = still_playing();
    send_to_all_clients(0);
    return result;
}

void move_player(PlayerData &p) {

    if (p.turn_direction == 1)
        p.direction += turning_speed;
    if (p.turn_direction == 2)
        p.direction -= turning_speed;

    p.direction = (p.direction) % 360;

    double direction_radians = (double) p.direction * DEG_TO_RADIANS;
    p.x += cos(direction_radians);
    p.y += sin(direction_radians);
}

//true if game has NOT ended
bool one_round() {
    size_t events_before = current_game.events.size();
    for (uint i = 0; i < current_game.players.size(); i++) {
        auto &player = *(current_game.players[i]);
        if (player.eliminated)
            continue;
        uint32_t last_x = player.x;
        uint32_t last_y = player.y;
        move_player(player);
        uint32_t x = player.x;
        uint32_t y = player.y;
        if (last_x == x && last_y == y)
            continue;
        else {
            if (x >= maxx || y >= maxy || current_game.board[x][y])
                generate_player_eliminated(i);
            else
                generate_pixel(x, y, i);
        }
        if (!still_playing()) {
            send_to_all_clients(events_before);
            return false;
        }
    }
    send_to_all_clients(events_before);
    return true;
}

//waits till the end of turn
void wait_to_end(uint64_t last_start, uint64_t time_per_round) {
    uint64_t time_passed = current_time_in_microseconds() - last_start;
    if (time_passed < time_per_round)
        usleep((time_per_round - time_passed));
}

// simulates rounds in loop
[[noreturn]] void do_rounds() {
    uint64_t last_start;
    uint64_t time_per_round = 1000000 / rounds_per_second;

    unique_lock<mutex> waiting_for_players(wait_for_players_mut);
    for (;;) {
        wait_for_players.wait(waiting_for_players, time_to_start);
        last_start = current_time_in_microseconds();
        {
            lock_guard<mutex> lock(mut);
            if (!start_game())
                continue;
        }
        wait_to_end(last_start, time_per_round);
        for (;;) {
            last_start = current_time_in_microseconds();
            {
                lock_guard<mutex> lock(mut);
                disconnect_old(current_time_in_microseconds());
                if (!one_round()) {
                    break;
                }
            }
            wait_to_end(last_start, time_per_round);
        }
    }
}

int main(int argc, char **argv) {
    ready_players = connected_players = 0;
    parse_options(argc, argv);
    validity_check();
    init_socket();
    thread listener(do_listen);
    do_rounds();
    listener.join();
    if (close(sock_fd) != 0) syserr("close");
    return 0;
}
