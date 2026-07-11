#include "client.h"

#include <netdb.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

uint8_t pack_coordinate(const char* coord_str);
int unpack_coordinate(uint8_t coord, char* coord_str);

int pack_place_message(PlaceMessage* msg, const struct Ship* ships);
int pack_attack_message(AttackMessage* msg, const char* coord_str);

TurnResult network_to_engine_result(uint8_t net_cmd);

// Changed to opaque pointer
typedef struct ClientImplementation {
    int socket_fd;
} ClientImplementation;

ClientImplementation* client_init() {
    ClientImplementation* ptr = malloc(sizeof(ClientImplementation));
    if (ptr != NULL) {
        // Init with invalid socket
        ptr->socket_fd = -1;
    }
    return ptr;
}

bool client_connect(ClientImplementation* client, const char* addr,
                    uint16_t port, uint32_t game_id) {
    char port_str[6];
    snprintf(port_str, sizeof(port_str), "%u", port);

    struct addrinfo hints, *res, *rp;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;        // IPv4
    hints.ai_socktype = SOCK_STREAM;  // TCP

    // Resolve host
    if (getaddrinfo(addr, port_str, &hints, &res) != 0) {
        return false;
    }

    int sockfd = -1;
    // Iterate through addresses until we successfully connect
    for (rp = res; rp != NULL; rp = rp->ai_next) {
        sockfd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (sockfd == -1) continue;

        // Successful connection
        if (connect(sockfd, rp->ai_addr, rp->ai_addrlen) != -1) {
            break;
        }
        close(sockfd);
    }
    freeaddrinfo(res);

    // Failed to connect
    if (rp == NULL) {
        return false;
    }

    client->socket_fd = sockfd;

    // Sends game_id to server in network byte order
    uint32_t net_game_id = htonl(game_id);
    if (send(client->socket_fd, &net_game_id, sizeof(uint32_t), MSG_NOSIGNAL) <
        0) {
        return false;
    }

    return true;
}

bool client_wait_for_opponent(ClientImplementation* client) {
    if (client == NULL || client->socket_fd < 0) {
        return false;
    }

    uint8_t ready_signal = 0;
    // Blocks until the server sends a byte
    if (recv(client->socket_fd, &ready_signal, sizeof(uint8_t), 0) <= 0) {
        // Drops connection on error
        return false;
    }

    // Server sends CMD_GAME_READY
    if (ready_signal == CMD_GAME_READY) {
        return true;
    }
    return false;
}

int8_t client_send_ships(ClientImplementation* client,
                         const struct Ship (*ships)[4]) {
    // Error
    if (client == NULL || client->socket_fd < 0 || ships == NULL) {
        return -1;
    }

    PlaceMessage msg;
    // Pack message
    if (pack_place_message(&msg, *ships)) {
        return -1;
    }

    // Send the 9 bytes to the server
    if (send(client->socket_fd, &msg, sizeof(PlaceMessage), MSG_NOSIGNAL) < 0) {
        return -1;
    }

    // Srver replies with the assigned player number
    int8_t player_num = -1;
    if (recv(client->socket_fd, &player_num, sizeof(int8_t), 0) <= 0) {
        return -1;
    }

    return player_num;
}

TurnResult client_send_move(ClientImplementation* client,
                            const char* coordinate) {
    // Error
    if (client == NULL || client->socket_fd < 0 || coordinate == NULL) {
        return Invalid;
    }

    AttackMessage msg;
    // Pack message
    if (pack_attack_message(&msg, coordinate)) {
        return Invalid;
    }

    // Send the 2 bytes to the server
    if (send(client->socket_fd, &msg, sizeof(AttackMessage), MSG_NOSIGNAL) <
        0) {
        return Invalid;
    }

    uint8_t return_num = 0;
    if (recv(client->socket_fd, &return_num, sizeof(uint8_t), 0) <= 0) {
        return Invalid;
    }

    // Takes server return and converts it to TurnResult enum
    return network_to_engine_result(return_num);
}

ExtendedTurnResult client_send_move_extended(ClientImplementation* client,
                                             const char* coordinate) {
    ExtendedTurnResult result = {
        .length = 0,
        .data = NULL,
    };

    // Safety checks
    if (client == NULL || client->socket_fd < 0 || coordinate == NULL) {
        return result;
    }

    // Create a dummy attack message
    AttackMessage msg;
    if (pack_attack_message(&msg, coordinate)) {
        return result;
    }

    // Override the attack command to be extended
    msg.command = CMD_ATTACK_EXT;

    // Send the extended attack message and recieve the payload
    if (send(client->socket_fd, &msg, sizeof(AttackMessage), MSG_NOSIGNAL) <
        0) {
        return result;
    }

    uint16_t net_len = 0;
    if (recv(client->socket_fd, &net_len, sizeof(uint16_t), 0) <= 0) {
        return result;
    }
    // Network byte order
    result.length = ntohs(net_len);

    if (result.length > 0) {
        result.data = malloc(result.length);
        if (result.data == NULL) {
            return result;
        }

        // Dynamically load the payload
        uint16_t total_received = 0;
        while (total_received < result.length) {
            int bytes =
                recv(client->socket_fd, (char*)result.data + total_received,
                     result.length - total_received, 0);

            // Server crash
            if (bytes <= 0) {
                free((void*)result.data);
                result.data = NULL;
                result.length = 0;
                return result;
            }
            total_received += bytes;
        }
    }

    return result;
}

MoveResult client_receive_move(ClientImplementation* client) {
    MoveResult result = {
        .coordinate = NULL,
        .result = Invalid,
    };

    if (client == NULL || client->socket_fd < 0) {
        return result;
    }

    NotifyMessage noti;
    int bytes_read = recv(client->socket_fd, &noti, sizeof(NotifyMessage), 0);
    if (bytes_read == sizeof(NotifyMessage)) {
        // Allocate 4 bytes for the string
        char* coord_str = malloc(4 * sizeof(char));
        if (coord_str != NULL) {
            // Unpack the byte back into the allocated string
            unpack_coordinate(noti.coord, coord_str);
            result.coordinate = coord_str;
        }

        // Convert the network hex back to the engine enum
        result.result = network_to_engine_result(noti.result);
    }

    return result;
}

void client_free_extended_result(ExtendedTurnResult result) {
    if (result.data != NULL) {
        // Remove const qualifier
        free((void*)result.data);
    }
}

void client_free_move_result(MoveResult result) {
    if (result.coordinate != NULL) {
        // Remove const qualifier
        free((void*)result.coordinate);
    }
}

void client_close(ClientImplementation* client) {
    if (client != NULL) {
        if (client->socket_fd >= 0) {
            // Close the network connection
            close(client->socket_fd);
        }
        free(client);
    }
}

// Converts string coordinate to my 8 bit representation
uint8_t pack_coordinate(const char* coord_str) {
    // Early check returns impossible byte representation
    if (coord_str == NULL || strlen(coord_str) > 3) {
        return 0xFF;
    }

    uint8_t col = coord_str[0] - 'A';
    if (col > 10) {
        return 0xFF;
    }

    // 0 based index
    uint8_t row = 0;

    // Case where number is 10
    if (coord_str[1] == '1' && coord_str[2] == '0') {
        row = 9;
    } else {
        row = coord_str[1] - '1';
    }

    if (row > 10) {
        return 0xFF;
    }

    return (col << 4) | (row & 0x0F);
}

// Unpacks an 8-bit coord back to a string
int unpack_coordinate(uint8_t coord, char* coord_str) {
    uint8_t col = coord >> 4;
    uint8_t row = coord & 0x0F;

    // Bounds checking, col / row cant be negative
    if (col > 9 || row > 9) {
        return 1;
    }

    // Converts to string format
    coord_str[0] = 'A' + col;
    if (row == 9) {
        coord_str[1] = '1';
        coord_str[2] = '0';
        coord_str[3] = '\0';
    } else {
        coord_str[1] = '1' + row;
        coord_str[2] = '\0';
    }

    return 0;
}

int pack_place_message(PlaceMessage* msg, const struct Ship* ships) {
    if (msg == NULL || ships == NULL) {
        return 1;
    }

    msg->command = CMD_PLACE;
    uint8_t dir_len;
    uint8_t coord;
    for (int i = 0; i < SHIPS; i++) {
        coord = pack_coordinate(ships[i].coordinate);
        if (coord == 0xFF) {
            return 1;
        }
        dir_len = 0x0;
        dir_len |= (uint8_t)ships[i].direction << 4;
        dir_len |= ((uint8_t)ships[i].length & 0x07);  // Keep last 3 bits
        msg->ships[i].coord = coord;
        msg->ships[i].dir_len = dir_len;
    }

    return 0;
}

int pack_attack_message(AttackMessage* msg, const char* coord_str) {
    if (msg == NULL || coord_str == NULL) {
        return 1;
    }
    uint8_t coord = pack_coordinate(coord_str);
    if (coord == 0xFF) {
        return 1;
    }
    msg->command = CMD_ATTACK;
    msg->coord = coord;

    return 0;
}

TurnResult network_to_engine_result(uint8_t net_cmd) {
    switch (net_cmd) {
        case CMD_MISS:
            return Miss;
        case CMD_HIT:
            return Hit;
        case CMD_WIN:
            return Win;
        case CMD_SUNK_2:
            return Sunk2;
        case CMD_SUNK_3:
            return Sunk3;
        case CMD_SUNK_4:
            return Sunk4;
        case CMD_SUNK_5:
            return Sunk5;
        case CMD_INVALID:
        default:
            return Invalid;
    }
}
