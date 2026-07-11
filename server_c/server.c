#include "server.h"

#include <assert.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "engine.h"

#define MULTIPLE_GAMES

typedef struct {
    int fd;
    uint64_t conn_id;
} ConnArg;

typedef struct GameSession {
    uint32_t game_id;
    int pending_fd;
    uint64_t conn_id;   // Ticket number to prevent race conditions
    uint8_t is_active;  // Both clients have joined
    struct GameSession* next;
} GameSession;

// Global variables so threads can access
GameSession* games = NULL;  // The Lobby Linked List
pthread_mutex_t lobby_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t engine_mutex = PTHREAD_MUTEX_INITIALIZER;

Engine* global_engine = NULL;

int unpack_coordinate(uint8_t coord, char* coord_str);
int unpack_ships(PlaceMessage packed, Ship* ships, char buffer[SHIPS][4]);

void* handle_connection(void* arg);
void play_game(int p1_fd, int p2_fd, uint32_t game_id);

uint8_t engine_to_network_result(TurnResult result);

void reset_server(int player1_fd, int player2_fd, Engine* engine,
                  uint32_t game_id);

// Functions for working with linked lists
void pop_game_by_id(uint32_t game_id);
GameSession* find_game_by_id(GameSession* list, uint32_t game_id);

int main(int argc, char* argv[]) {
    // Check command line arguments
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <port>\n", argv[0]);
        exit(1);  // Exit status 1 if port is not provided
    }

    // Init server engine
    global_engine = engine_init();
    if (global_engine == NULL) {
        exit(2);
    }

    // Setup server socket
    int port = atoi(argv[1]);
    int server_fd;
    struct sockaddr_in server_addr;

    // Create TCP socket
    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("Error opening socket");
        exit(3);  // Exit status 3 for socket failures
    }

    // Code per spec prior to bind call
    int enable = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(int)) <
        0) {
        perror("setsockopt");
        exit(3);
    }

    // Bind socket to the specified port on any interface
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(port);

    if (bind(server_fd, (struct sockaddr*)&server_addr, sizeof(server_addr)) <
        0) {
        perror("Error on binding");
        exit(3);
    }

    // Listen for incoming connections
    if (listen(server_fd, 5) < 0) {
        perror("Error on listen");
        exit(3);
    }

    printf("Server listening on port %d...\n", port);

    uint64_t global_conn_id = 0;
    while (1) {
        // Listen for clients
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);

        int client_fd =
            accept(server_fd, (struct sockaddr*)&client_addr, &client_len);
        if (client_fd < 0) {
            continue;
        }

        // Track current connection ticket
        ConnArg* arg = malloc(sizeof(ConnArg));
        if (arg == NULL) {
            continue;
        }
        arg->fd = client_fd;
        arg->conn_id = global_conn_id++;

        // Thread to handle the connection
        pthread_t tid;
        pthread_create(&tid, NULL, handle_connection, arg);
        // OS handles cleaning for threads
        pthread_detach(tid);
    }

    // Good to clean :)
    close(server_fd);
    engine_free(global_engine);
    return 0;
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

// Unpacks ships from a signal
int unpack_ships(PlaceMessage packed, Ship* ships, char buffer[SHIPS][4]) {
    if (ships == NULL) {
        return 1;
    }

    for (int i = 0; i < SHIPS; i++) {
        // Points coord to buffer
        ships[i].coordinate = buffer[i];

        // Unpacks coord to buffer
        if (unpack_coordinate(packed.ships[i].coord, buffer[i])) {
            return 1;
        }

        // Sets direction and length
        ships[i].direction = (packed.ships[i].dir_len >> 4) & 0x01;
        ships[i].length = packed.ships[i].dir_len & 0x07;
        if (ships[i].length > 5) {
            return 1;
        }
    }

    return 0;
}

// Handles setting up connections
void* handle_connection(void* arg) {
    ConnArg* conn = (ConnArg*)arg;
    if (conn == NULL) {
        return NULL;
    }
    int client_fd = conn->fd;
    uint64_t curr_conn_id = conn->conn_id;
    free(conn);

    // Read game id
    uint32_t net_id;
    if (recv(client_fd, &net_id, sizeof(uint32_t), 0) <= 0) {
        // Malicious / broken client dropped
        close(client_fd);
        return NULL;
    }
    // Convert byte order
    uint32_t game_id = ntohl(net_id);

    // Check lobby safely
    pthread_mutex_lock(&lobby_mutex);

    // Search lobby for game id
    GameSession* curr = find_game_by_id(games, game_id);
    if (curr != NULL && curr->is_active) {
        printf("Game %u currently in use\n", game_id);
        close(client_fd);
        pthread_mutex_unlock(&lobby_mutex);
        return NULL;
    }

    // Create a new game and add it to the list of games
    if (curr == NULL) {
        // Lock engine and init game
        pthread_mutex_lock(&engine_mutex);
        bool init_success = engine_init_game(global_engine, game_id);
        pthread_mutex_unlock(&engine_mutex);

        // Close this game on failure
        if (!init_success) {
            printf("Failed to init game %u in engine.\n", game_id);
            close(client_fd);
            return NULL;
        }
        printf("Game %u: Player 1 joined. Waiting for Player 2...\n", game_id);

        GameSession* new_game = malloc(sizeof(GameSession));
        if (new_game == NULL) {
            return NULL;
        }
        new_game->game_id = game_id;
        new_game->pending_fd = client_fd;
        new_game->conn_id = curr_conn_id;
        new_game->is_active = 0;
        new_game->next = games;
        games = new_game;

        pthread_mutex_unlock(&lobby_mutex);
        return NULL;
    }

    // Connect as player 2 to existing game
    else {
        printf("Game %u: Player 2 joined. Starting match!\n", game_id);

        int other_fd = curr->pending_fd;
        uint64_t other_conn_id = curr->conn_id;
        curr->is_active = 1;

        // Removes game from pending list
        pthread_mutex_unlock(&lobby_mutex);

        // Safely sort P1 and P2 by ticket to see who connected first
        int p1_fd, p2_fd;
        if (curr_conn_id < other_conn_id) {
            p1_fd = client_fd;
            p2_fd = other_fd;
        } else {
            p1_fd = other_fd;
            p2_fd = client_fd;
        }

        // Both players connected, start game loop
        play_game(p1_fd, p2_fd, game_id);
        return NULL;
    }
}

// Handles game play logic
void play_game(int p1_fd, int p2_fd, uint32_t game_id) {
    // Send game ready singnal to both clients
    uint8_t ready_signal = CMD_GAME_READY;
    send(p1_fd, &ready_signal, sizeof(uint8_t), MSG_NOSIGNAL);
    send(p2_fd, &ready_signal, sizeof(uint8_t), MSG_NOSIGNAL);

    // Variable to track game attack loop
    int is_playing = 1;

    // --- Ship Placement ---
    PlaceMessage p1_msg, p2_msg;
    int bytes_read = 0;

    // Player 1
    bytes_read = recv(p1_fd, &p1_msg, sizeof(PlaceMessage), 0);
    if (bytes_read == sizeof(PlaceMessage) && p1_msg.command == CMD_PLACE) {
        Ship p1_ships[SHIPS];
        char buffer[SHIPS][4];
        unpack_ships(p1_msg, p1_ships, buffer);

        // Register player 1 their assigned number safely
        pthread_mutex_lock(&engine_mutex);
        int8_t assigned_p1 =
            (int8_t)engine_place_ships(global_engine, game_id, &p1_ships);
        pthread_mutex_unlock(&engine_mutex);

        // Player 1 placing error, notify player 2 and close server
        if (assigned_p1 == -1) {
            printf("Game %u: Player 1 invalid ship placement. Aborting.\n",
                   game_id);
            int8_t err = -1;
            send(p1_fd, &err, sizeof(int8_t), MSG_NOSIGNAL);
            send(p2_fd, &err, sizeof(int8_t), MSG_NOSIGNAL);
            reset_server(p1_fd, p2_fd, global_engine, game_id);
            return;
        }
        send(p1_fd, &assigned_p1, sizeof(int8_t), MSG_NOSIGNAL);
    } else {
        // Error or disconnect
        int8_t err = -1;
        send(p2_fd, &err, sizeof(int8_t), MSG_NOSIGNAL);
        reset_server(p1_fd, p2_fd, global_engine, game_id);
        is_playing = 0;
        return;
    }

    // Player 2
    bytes_read = recv(p2_fd, &p2_msg, sizeof(PlaceMessage), 0);
    if (bytes_read == sizeof(PlaceMessage) && p2_msg.command == CMD_PLACE) {
        Ship p2_ships[SHIPS];
        char buffer[SHIPS][4];
        unpack_ships(p2_msg, p2_ships, buffer);

        // Register player 2 their assigned number safely
        pthread_mutex_lock(&engine_mutex);
        int8_t assigned_p2 =
            (int8_t)engine_place_ships(global_engine, game_id, &p2_ships);
        pthread_mutex_unlock(&engine_mutex);

        // Player 2 placing error, notify player 1 and close server
        if (assigned_p2 == -1) {
            printf("Game %u: Player 1 invalid ship placement. Aborting.\n",
                   game_id);
            int8_t err = -1;
            send(p1_fd, &err, sizeof(int8_t), MSG_NOSIGNAL);
            send(p2_fd, &err, sizeof(int8_t), MSG_NOSIGNAL);
            reset_server(p1_fd, p2_fd, global_engine, game_id);
            return;
        }
        send(p2_fd, &assigned_p2, sizeof(int8_t), MSG_NOSIGNAL);
    } else {
        // Error or disconnect
        int8_t err = -1;
        send(p1_fd, &err, sizeof(int8_t), MSG_NOSIGNAL);
        reset_server(p1_fd, p2_fd, global_engine, game_id);
        is_playing = 0;
        return;
    }

    if (is_playing == 0) {
        uint8_t error = CMD_ERROR;
        send(p1_fd, &error, sizeof(uint8_t), MSG_NOSIGNAL);
        send(p2_fd, &error, sizeof(uint8_t), MSG_NOSIGNAL);
    }

    printf("Game %u: Ships placed! Starting gameplay loop...\n", game_id);

    int active_player = 1;
    while (is_playing) {
        // Determines who is turn it is
        int current_fd = (active_player == 1) ? p1_fd : p2_fd;
        int waiting_fd = (active_player == 1) ? p2_fd : p1_fd;

        AttackMessage attack_msg;
        int bytes_read =
            recv(current_fd, &attack_msg, sizeof(AttackMessage), 0);

        // Player disconnects or empty send
        if (bytes_read <= 0) {
            printf("Game %u: Player %d disconnected.\n", game_id,
                   active_player);

            // Send dummy noti so waiting player unblocks
            NotifyMessage noti = {.coord = 0xFF, .result = CMD_INVALID};
            send(waiting_fd, &noti, sizeof(NotifyMessage), MSG_NOSIGNAL);
            break;
        }

        if (bytes_read == sizeof(AttackMessage) &&
            (attack_msg.command == CMD_ATTACK ||
             attack_msg.command == CMD_ATTACK_EXT)) {
            // Convert coord
            char coord_buffer[4];
            unpack_coordinate(attack_msg.coord, coord_buffer);
            TurnResult engine_res;

            // Normal attack
            if (attack_msg.command == CMD_ATTACK) {
                // Get result from engine safely
                pthread_mutex_lock(&engine_mutex);
                engine_res = engine_take_turn(global_engine, game_id,
                                              active_player, coord_buffer);
                pthread_mutex_unlock(&engine_mutex);

                // Convert engine result and send signal
                int8_t net_res = engine_to_network_result(engine_res);
                send(current_fd, &net_res, sizeof(uint8_t), MSG_NOSIGNAL);

            }
            // Extended attack
            else {
                // Get extended result from engine safely
                pthread_mutex_lock(&engine_mutex);
                ExtendedTurnResult ext_res = engine_take_turn_extended(
                    global_engine, game_id, active_player, coord_buffer);
                pthread_mutex_unlock(&engine_mutex);

                // Send the 16bit length in network byte order
                uint16_t net_len = htons(ext_res.length);
                send(current_fd, &net_len, sizeof(uint16_t), MSG_NOSIGNAL);

                // Send the raw data
                if (ext_res.length > 0 && ext_res.data != NULL) {
                    send(current_fd, ext_res.data, ext_res.length,
                         MSG_NOSIGNAL);
                }

                // Extract the result
                engine_res = engine_extract_turn_result(ext_res);
                engine_free_extended_result(ext_res);
            }

            if (engine_res != Invalid) {
                // Create and send server notification
                NotifyMessage noti = {
                    .coord = attack_msg.coord,
                    .result = engine_to_network_result(engine_res)};
                send(waiting_fd, &noti, sizeof(NotifyMessage), MSG_NOSIGNAL);

                if (engine_res == Win) {
                    printf("Game %u: Player %d wins!\n", game_id,
                           active_player);
                    is_playing = 0;
                } else {
                    // Swaps active player
                    active_player = (active_player == 1) ? 2 : 1;
                }
            }
        } else {
            break;
        }
    }

    reset_server(p1_fd, p2_fd, global_engine, game_id);
    printf("Game %u ended. Resetting...\n", game_id);
    return;
}

// Converts engine singals to my network signals
uint8_t engine_to_network_result(TurnResult result) {
    switch (result) {
        case Miss:
            return CMD_MISS;
        case Hit:
            return CMD_HIT;
        case Win:
            return CMD_WIN;
        case Sunk2:
            return CMD_SUNK_2;
        case Sunk3:
            return CMD_SUNK_3;
        case Sunk4:
            return CMD_SUNK_4;
        case Sunk5:
            return CMD_SUNK_5;
        case Invalid:
        default:
            return CMD_INVALID;
    }
}

// Resets the engine and closes connection for a game
void reset_server(int player1_fd, int player2_fd, Engine* engine,
                  uint32_t game_id) {
    if (player1_fd >= 0) close(player1_fd);
    if (player2_fd >= 0) close(player2_fd);
    pop_game_by_id(game_id);

    // Lock engine before ending game
    if (engine != NULL) {
        pthread_mutex_lock(&engine_mutex);
        engine_end_game(engine, game_id);
        pthread_mutex_unlock(&engine_mutex);
    }
    return;
}

// Removes game from linked list
void pop_game_by_id(uint32_t game_id) {
    // Acquires lock and finds the game
    pthread_mutex_lock(&lobby_mutex);
    GameSession* prev = NULL;
    GameSession* curr = games;
    while (curr != NULL) {
        if (curr->game_id == game_id) {
            break;
        }
        prev = curr;
        curr = curr->next;
    }
    // Removes from the list and frees
    if (curr != NULL) {
        if (prev == NULL) {
            games = curr->next;
        } else {
            prev->next = curr->next;
        }
        free(curr);
    }
    pthread_mutex_unlock(&lobby_mutex);
    return;
}

// Finds game by its game_id in linked list
GameSession* find_game_by_id(GameSession* list, uint32_t game_id) {
    GameSession* curr = list;
    while (curr != NULL) {
        if (curr->game_id == game_id) {
            break;
        }
        curr = curr->next;
    }
    return curr;
}
