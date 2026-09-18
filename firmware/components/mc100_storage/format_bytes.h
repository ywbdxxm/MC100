#ifndef MC100_FORMAT_BYTES_H
#define MC100_FORMAT_BYTES_H

#include <stdint.h>

static inline void mc100_put_u16(uint8_t *p, uint16_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
}

static inline void mc100_put_u32(uint8_t *p, uint32_t value)
{
    for (unsigned i = 0; i < 4; ++i) p[i] = (uint8_t)(value >> (8 * i));
}

static inline void mc100_put_u64(uint8_t *p, uint64_t value)
{
    for (unsigned i = 0; i < 8; ++i) p[i] = (uint8_t)(value >> (8 * i));
}

static inline uint16_t mc100_get_u16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static inline uint32_t mc100_get_u32(const uint8_t *p)
{
    uint32_t value = 0;
    for (unsigned i = 0; i < 4; ++i) value |= (uint32_t)p[i] << (8 * i);
    return value;
}

static inline uint64_t mc100_get_u64(const uint8_t *p)
{
    uint64_t value = 0;
    for (unsigned i = 0; i < 8; ++i) value |= (uint64_t)p[i] << (8 * i);
    return value;
}

#endif
