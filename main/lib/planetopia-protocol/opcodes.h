/* Copied from: planetopia-protocol/c/opcodes.h
 * Source repo: https://github.com/benjaminswanepoel/planetopia-protocol
 * Keep in sync with that repo — do not edit values here directly.
 * For production, set up a git submodule or CI step to keep this file current. */

#pragma once

/* Planetopia mesh protocol opcode constants.
 * Keep in sync with opcodes/opcodes.go in this repo. */

/* Serial command opcodes — byte 0 of data payload */
#define OP_NODE_ID_SET   0xC0
#define OP_CONFIG_SET    0xC1
#define OP_TX_POWER_SET  0xC2

/* Output adapter commands (server → output node) */
#define OP_LED_SOLID     0xD0
#define OP_LED_OFF       0xD1
#define OP_LED_BLINK     0xD2
#define OP_RELAY_SET     0xD8

/* Input adapter events / acknowledgements */
#define OP_COMMAND_ACK   0xE0
