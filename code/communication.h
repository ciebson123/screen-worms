//
// Created by kamil on 08.05.2021.
//

#ifndef ZADANIE2_COMMUNICATION_H
#define ZADANIE2_COMMUNICATION_H

#include <cstdint>

#define DEFAULT_SERWER_PORT 2021
#define DEFAULT_SERWER_PORT_STR "2021"
#define DEFAULT_TURNING_SPEED 6
#define DEFAULT_ROUNDS_PER_SECOND 50
#define DEFAULT_WIDTH 640
#define DEFAULT_HEIGHT 480

#define MAX_WIDTH 4000
#define MAX_HEIGHT 4000
#define MAX_ROUND_PER_SECOND 200
#define MAX_TURNING_SPEED 359
#define MAX_PLAYER_NAME_LENGTH 20
#define MAX_HOST_MESS_LEN 550

#define DEFAULT_GUI_SERVER "localhost"
#define DEFAULT_GUI_PORT "20210"
#define LEFT 2
#define RIGHT 1

using client_to_serwer_mess = struct __attribute__((__packed__)) client_to_serwer {
    uint64_t session_id;
    uint8_t turn_direction;
    uint32_t next_expected_event_no;
    char player_name[MAX_PLAYER_NAME_LENGTH + 1];
};

#define CLIENT_HEADER_SIZE 13

using event_header_mess =  struct __attribute__((__packed__)) event_header {
    uint32_t len;
    uint32_t event_no;
    uint8_t event_type;
    event_header(uint32_t len, uint32_t event_no, uint8_t event_type) : len(len), event_no(event_no),
        event_type(event_type) {}
};

#define EVENT_NO_TYPE_SIZE 5
#define EVENT_HEADER_SIZE 9
#define EVENT_HEADER_META 13

#define NEW_GAME_TYPE 0

using new_game_data_mess = struct __attribute__((__packed__)) new_game_data {
    uint32_t maxx;
    uint32_t maxy;
};

#define NEW_GAME_EVENT_MINIMUMLEN (8 + EVENT_NO_TYPE_SIZE)

#define PIXEL_TYPE 1

using pixel_data_mess = struct __attribute__((__packed__)) pixel_data {
    uint8_t player_number;
    uint32_t x;
    uint32_t y;
};

#define PIXEL_DATA_LEN (9 + EVENT_NO_TYPE_SIZE)

#define ELIMINATED_TYPE 2

using eliminated_data_mess = struct __attribute__((__packed__)) eliminated_data {
    uint8_t player_number;
};

#define ELIMINATED_DATA_LEN (1 + EVENT_NO_TYPE_SIZE)

#define END_GAME_TYPE 3

#define END_GAME_DATA_LEN EVENT_NO_TYPE_SIZE

#endif //ZADANIE2_COMMUNICATION_H
