#include "crc.h"

#define POLY_REV  0xEDB88320

/* using wikipedia algorithm https://en.wikipedia.org/wiki/Computation_of_cyclic_redundancy_checks */

crc32_t crc_cacl(uint8_t message[], uint32_t size) {
    crc32_t rem  = -1;
    for (uint32_t i = 0; i < size; i++) {
        rem  ^= message[i];
        for (int j = 0; j < 8; j++) {
            if (rem & 1) {
                rem  = (rem >> 1) ^ POLY_REV;
            } else {
                rem  = rem >> 1;
            }
        }
    }
    return ~rem;
}