#include "checksum.h"

/*
 * Inicia o acumulador CRC32.
 */
uint32_t crc32_init(void) {
    return 0xFFFFFFFFU;
}

/*
 * Atualiza o CRC32 com uma porção de bytes.
 *
 * O sender chama essa função enquanto lê o arquivo local.
 * O receiver chama essa função ao reler o arquivo .part no FINISH.
 */
uint32_t crc32_update(
    uint32_t crc,
    const unsigned char *data,
    size_t length
) {
    for (size_t i = 0; i < length; ++i) {
        crc ^= data[i];

        for (int bit = 0; bit < 8; ++bit) {
            if (crc & 1U) {
                crc = (crc >> 1) ^ 0xEDB88320U;
            } else {
                crc >>= 1;
            }
        }
    }

    return crc;
}

/*
 * Finaliza o CRC32.
 */
uint32_t crc32_finalize(uint32_t crc) {
    return crc ^ 0xFFFFFFFFU;
}