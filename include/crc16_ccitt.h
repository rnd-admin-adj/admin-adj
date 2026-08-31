#ifndef CRC16_CCITT_H
#define CRC16_CCITT_H

#include <stdint.h>

/* CRC-16/CCITT-FALSE: poly 0x1021, init 0xFFFF.
   Matches server.py's crc16_ccitt() exactly. */
uint16_t crc16_ccitt(const uint8_t *data, uint16_t len);

#endif