#ifndef CRC8_H
#define CRC8_H

#include <stdint.h>

/* Simple CRC-8, polynomial 0x07 (CRC-8-CCITT).
   Used to sanity-check ADXL345 X/Y/Z payload over TCP. */
uint8_t crc8(const uint8_t *data, uint16_t len);

#endif