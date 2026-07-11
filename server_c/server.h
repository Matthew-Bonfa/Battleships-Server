#ifndef SERVER_H
#define SERVER_H

#include <stdint.h>

#define SHIPS 4

// --- Server Side Returns ---
#define CMD_INVALID 0x0  // 0000 0000
#define CMD_HIT 0x01     // 0000 0001
#define CMD_MISS 0x02    // 0000 0010

#define CMD_SUNK_2 0x23  // 0010 0011
#define CMD_SUNK_3 0x33  // 0011 0011
#define CMD_SUNK_4 0x43  // 0100 0011
#define CMD_SUNK_5 0x53  // 0101 0011

#define CMD_WIN 0x04   // 0000 0100
#define CMD_LOSE 0x05  // 0000 0101

#define CMD_GAME_READY 0x07  // 0000 0111
#define CMD_ERROR 0xFF       // 1111 1111

// --- Client Side Requests ---
#define CMD_PLACE 0x09       // 0000 1001
#define CMD_ATTACK 0x0A      // 0000 1010
#define CMD_ATTACK_EXT 0x0B  // 0000 1011

// 2-byte ship
typedef struct __attribute__((packed)) {
    uint8_t coord;    // 4 bits col | 4 bits row
    uint8_t dir_len;  // 4 bits dir | 4 bits len
} NetShip;

// 9-byte PLACE command
typedef struct __attribute__((packed)) {
    uint8_t command;       // CMD_PLACE
    NetShip ships[SHIPS];  // 4 ships * 2 bytes = 8 bytes
} PlaceMessage;

// 2-byte ATTACK command
typedef struct __attribute__((packed)) {
    uint8_t command;  // CMD_ATTACK
    uint8_t coord;    // 4 bits col | 4 bits row
} AttackMessage;

// 2-byte return from server
typedef struct __attribute__((packed)) {
    uint8_t coord;   // Coordinate that was attacked
    uint8_t result;  // Outcome
} NotifyMessage;

#endif
