#ifndef ZADANIE2_CRC_H
#define ZADANIE2_CRC_H

#include <cstdint>

using crc32_t = uint32_t;

crc32_t crc_cacl(uint8_t message[], uint32_t size);


#endif //ZADANIE2_CRC_H
