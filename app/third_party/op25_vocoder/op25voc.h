#pragma once
// C wrapper around OP25's AMBE/IMBE encoders (GPLv3, see COPYING).
// Only these symbols are exported from op25voc.o; OP25's own copy of
// mbelib stays private so it can't clash with third_party/mbelib.
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
void *op25voc_ambe_new(int dstar);   // 0: 49-bit AMBE+2 (3600x2450), 1: D-Star AMBE (72 bits, interleaved), 2: D-Star, alternate interleave
// 160 samples (8 kHz, 20 ms) -> out: 49 bits or 72 bits, one bit per byte.
void op25voc_ambe_encode(void *h, const int16_t *samples160, uint8_t *outBits);
void op25voc_ambe_free(void *h);
void *op25voc_imbe_new(void);
// 160 samples -> IMBE u0..u7 (12,12,12,12,11,11,11,7 bits).
void op25voc_imbe_encode(void *h, const int16_t *samples160, int16_t u[8]);
void op25voc_imbe_free(void *h);
#ifdef __cplusplus
}
#endif
