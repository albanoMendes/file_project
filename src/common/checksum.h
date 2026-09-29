#ifndef CHECKSUM_H
#define CHECKSUM_H

#include <stddef.h>
#include <stdint.h>

/*
 * CRC32 incremental.
 *
 * Uso:
 *
 * uint32_t crc = crc32_init();
 * crc = crc32_update(crc, bytes1, size1);
 * crc = crc32_update(crc, bytes2, size2);
 * crc = crc32_finalize(crc);
 */
uint32_t crc32_init(void);

uint32_t crc32_update(
    uint32_t crc,
    const unsigned char *data,
    size_t length
);

uint32_t crc32_finalize(uint32_t crc);

#endif