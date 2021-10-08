#include <iostream>
#include <cstdint>
#include <sys/socket.h>
#include <unistd.h>
#include <cstring>
#include <thread>
#include <netinet/in.h>
#include <algorithm>
#include <atomic>
#include <mutex>
#include <unordered_map>
#include <netdb.h>
#include <vector>
#include <sys/time.h>
#include <netinet/tcp.h>
#include "communication.h"
#include "err.h"
#include "crc.h"

using namespace std;

#define BUF_SIZE 600
#define MESSAGE_SERVER_TIME 20000

const char *options = "n:p:i:r:";
int sock_serwer, sock_gui;
string port_serwer = DEFAULT_SERWER_PORT_STR,
        port_gui = DEFAULT_GUI_PORT,
        gui_serwer = DEFAULT_GUI_SERVER,
        player_name = "";

atomic<uint8_t> turn_direction(0);
atomic<uint32_t> next_expeced_event_no(0);
int64_t current_game_id = -1;
vector<string> player_names;
uint32_t maxx(0);
uint32_t maxy(0);
uint32_t old_game_id;

void parse_options(int argc, char **argv) {
    int c;
    while ((c = getopt(argc, argv, options)) != -1) {
        switch (c) {
            case 'p':
                port_serwer = optarg;
                break;
            case 'n':
                player_name = optarg;
                break;
            case 'i':
                gui_serwer = optarg;
                break;
            case 'r':
                port_gui = optarg;
                break;
            default:
                fatal("UNKNOWN OPTION");
        }
    }
}

addrinfo get_hints(int socktype, int protocol) {
    addrinfo addr_hints{};
    memset(&addr_hints, 0, sizeof(struct addrinfo));
    addr_hints.ai_flags = 0;
    addr_hints.ai_family = AF_UNSPEC;
    addr_hints.ai_socktype = socktype;
    addr_hints.ai_protocol = protocol;
    return addr_hints;
}

void init_serwer_conn(const string &serwer_name) {

    addrinfo addr_hints = get_hints(SOCK_DGRAM, 0), *addr_result;
    if (getaddrinfo(serwer_name.c_str(), port_serwer.c_str(),
                    &addr_hints, &addr_result) != 0)
        syserr("getaddrinfo");

    sock_serwer = socket(addr_result->ai_family, SOCK_DGRAM, 0);
    if (sock_serwer < 0) {
        syserr("socket");
    }

    if (connect(sock_serwer, addr_result->ai_addr, addr_result->ai_addrlen) != 0)
        syserr("connect");


    freeaddrinfo(addr_result);
}

void init_gui_conn() {


    addrinfo addr_hints = get_hints(SOCK_STREAM, IPPROTO_TCP), *addr_result;


    if (getaddrinfo(gui_serwer.c_str(), port_gui.c_str(),
                    &addr_hints, &addr_result) != 0)
        syserr("getaddrinfo");

    sock_gui = socket(addr_result->ai_family, SOCK_STREAM, IPPROTO_TCP);
    if (sock_gui < 0) {
        syserr("socket");
    }

    int flag = 1;
    if (setsockopt(sock_gui, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(int)) != 0)
        syserr("setsockopt");

    if (connect(sock_gui, addr_result->ai_addr, addr_result->ai_addrlen) != 0)
        syserr("connect");


    freeaddrinfo(addr_result);
}

void init_connections(const string &serwer_name) {
    init_serwer_conn(serwer_name);
    init_gui_conn();
}

uint64_t current_time_in_microseconds() {
    timeval curr_time{};
    gettimeofday(&curr_time, nullptr);
    return 1000000 * curr_time.tv_sec + curr_time.tv_usec;
}

//message server and wait for 20ms to pass
void serwer_message_and_wait(client_to_serwer_mess &message, int32_t size) {
    uint64_t last_time = current_time_in_microseconds();

    message.turn_direction = turn_direction;
    message.next_expected_event_no = htobe32(next_expeced_event_no);

    if (write(sock_serwer, &message, size) < size)
        syserr("write");

    uint64_t time_to_wait = MESSAGE_SERVER_TIME - (current_time_in_microseconds() - last_time);
    if (time_to_wait > 0)
        usleep(time_to_wait);
}

//message server in a loop
[[noreturn]] void send_to_serwer() {
    uint64_t session_id = current_time_in_microseconds();
    client_to_serwer_mess my_mess;
    my_mess.session_id = htobe64(session_id);
    memcpy(my_mess.player_name, player_name.c_str(), player_name.size());
    int32_t mess_size = CLIENT_HEADER_SIZE + player_name.size();

    for (;;) {
        serwer_message_and_wait(my_mess, mess_size);
    }
}

void check_command(const string &command) {
    if (command == "LEFT_KEY_DOWN")
        turn_direction = LEFT;
    if (command == "RIGHT_KEY_DOWN")
        turn_direction = RIGHT;
    if (command == "LEFT_KEY_UP" && turn_direction == LEFT)
        turn_direction = 0;
    if (command == "RIGHT_KEY_UP" && turn_direction == RIGHT)
        turn_direction = 0;
}

[[noreturn]] void listen_to_gui() {
    char buffer[BUF_SIZE];
    memset(buffer, 0, BUF_SIZE);
    string next_comm;
    for (;;) {
        int comm_size;
        if ((comm_size = read(sock_gui, buffer, 100)) < 0)
            syserr("read");
        for (int i = 0; i < comm_size; i++) {
            if (buffer[i] == '\n') {
                check_command(next_comm);
                next_comm.clear();
            } else {
                next_comm += buffer[i];
            }
        }
    }
}

uint32_t net_buffer_to_32(const char *buf) {
    return be32toh(*(uint32_t *) buf);
}

//check game id, true if id from last game, false if current/new id (sets new id if needed)
bool chack_and_set_id(int64_t game_id) {
    if (game_id == old_game_id)
        return true;
    if (current_game_id == game_id)
        return false;

    //got new game, start it
    next_expeced_event_no = 0;
    current_game_id = game_id;
    return false;

}

event_header_mess get_next_event(char *buff) {
    event_header_mess event = *(event_header_mess *) buff;
    event.event_no = be32toh(event.event_no);
    event.len = be32toh(event.len);

    return event;
}

bool bad_crc(char *message, int32_t event_len) {
    int32_t size = sizeof(uint32_t) + event_len; // sizeof(len) + lenght of event
    uint32_t crc = net_buffer_to_32(message + size);
    if (crc_cacl((uint8_t *) message, size) != crc)
        return true;
    return false;
}

string new_game(char *message, event_header_mess &header) {
    player_names.clear();
    char *new_player_names = message + EVENT_HEADER_SIZE + 8;

    maxx = net_buffer_to_32(message + EVENT_HEADER_SIZE);
    maxy = net_buffer_to_32(message + EVENT_HEADER_SIZE + 4);

    string result = "NEW_GAME " + to_string(maxx) + " " + to_string(maxy);
    string next_name;
    for (uint32_t i = 0; i < header.len - NEW_GAME_EVENT_MINIMUMLEN; i++) {
        if (new_player_names[i]) {
            next_name += new_player_names[i];
        } else {
            result += " " + next_name;
            player_names.push_back(next_name);
            next_name.clear();
        }
    }
    result += "\n";
    return result;
}

string pixel(char *message, event_header_mess &header) {
    if (header.len != PIXEL_DATA_LEN)
        fatal("BAD PIXEL DATA LENGHT");
    pixel_data_mess data = *(pixel_data_mess *) (message + EVENT_HEADER_SIZE);
    data.x = be32toh(data.x);
    data.y = be32toh(data.y);

    if (data.x >= maxx || data.y >= maxy || data.player_number >= player_names.size())
        fatal("PIXEL MAKES NO SENSE");

    return "PIXEL " + to_string(data.x) + " " + to_string(data.y)
           + " " + player_names[data.player_number] + "\n";

}

string eliminated(char *message, event_header_mess &header) {
    if (header.len != ELIMINATED_DATA_LEN)
        fatal("BAD ELIMINATED DATA LENGHT");
    eliminated_data_mess data = *(eliminated_data_mess *) (message + EVENT_HEADER_SIZE);
    if (data.player_number >= player_names.size())
        fatal("ELIMINATED MAKES NO SENSE");

    return "PLAYER_ELIMINATED " + player_names[data.player_number] + "\n";
}

string end_game(char *, event_header_mess &header) {
    if (header.len != END_GAME_DATA_LEN)
        fatal("BAD END DATA LENGHT");

    old_game_id = current_game_id;
    current_game_id = -1;


    return "";
}

string parse_event(char *message, event_header_mess &header) {
    if (next_expeced_event_no != header.event_no)
        return "";
    next_expeced_event_no++;
    switch (header.event_type) {
        case NEW_GAME_TYPE:
            return new_game(message, header);
        case PIXEL_TYPE:
            return pixel(message, header);
        case ELIMINATED_TYPE:
            return eliminated(message, header);
        case END_GAME_TYPE:
            return end_game(message, header);
        default:
            //ignoring
            return "";
    }
}
//parses one message from server to string
string parse_message(char *message, int32_t size) {
    string result = "";
    if (size < 4)
        return "";
    uint32_t game_id = net_buffer_to_32(message);
    message += 4;
    size -= 4;

    if (chack_and_set_id(game_id)) {
        return result;
    }

    while (size >= EVENT_HEADER_META) {
        event_header_mess next_event = get_next_event(message);

        if (bad_crc(message, next_event.len)) {
            break;
        }

        result += parse_event(message, next_event);
        message += (next_event.len - EVENT_NO_TYPE_SIZE + EVENT_HEADER_META);
        size -= (next_event.len - EVENT_NO_TYPE_SIZE + EVENT_HEADER_META);
    }
    return result;
}

[[noreturn]] void receive_and_send() {
    char buffer[BUF_SIZE];
    int read_size;
    for (;;) {
        if ((read_size = read(sock_serwer, buffer, BUF_SIZE)) < 0)
            syserr("read");
        if (read_size > MAX_HOST_MESS_LEN)
            fatal("MESSAGE FROM SERWER TOO LONG");

        string to_send = parse_message(buffer, read_size);

        if (to_send.empty())
            continue;

        if (write(sock_gui, to_send.c_str(), to_send.size()) < (ssize_t) to_send.size())
            syserr("write");

    }
}

void play() {
    thread sender(send_to_serwer);
    thread from_server_to_gui(receive_and_send);
    listen_to_gui();
}

int main(int argc, char **argv) {
    if (argc < 2)
        fatal("No serwer adress provided");

    string serwer_name = argv[1];
    parse_options(argc, argv);
    if (player_name.size() > 20)
        fatal("name too long");
    init_connections(serwer_name);
    play();
    return 0;
}
