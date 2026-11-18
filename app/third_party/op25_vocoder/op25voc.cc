#include "op25voc.h"
#include "imbe_vocoder/imbe_vocoder.h"
#include "mbelib.h"
#include "p25p2_vf.h"
#include "ambe_encoder.h"
#include <cstring>

extern "C" {
void *op25voc_ambe_new(int dstar)
{
    auto *e = new ambe_encoder;
    if (dstar) e->set_dstar_mode(); else e->set_49bit_mode();
    if (dstar == 2) e->set_alt_dstar_interleave(true);
    return e;
}
void op25voc_ambe_encode(void *h, const int16_t *samples160, uint8_t *outBits)
{
    int16_t buf[160];
    std::memcpy(buf, samples160, sizeof buf);
    static_cast<ambe_encoder *>(h)->encode(buf, outBits);
}
void op25voc_ambe_free(void *h) { delete static_cast<ambe_encoder *>(h); }
void *op25voc_imbe_new(void) { return new imbe_vocoder; }
void op25voc_imbe_encode(void *h, const int16_t *samples160, int16_t u[8])
{
    int16_t buf[160];
    std::memcpy(buf, samples160, sizeof buf);
    static_cast<imbe_vocoder *>(h)->imbe_encode(u, buf);
}
void op25voc_imbe_free(void *h) { delete static_cast<imbe_vocoder *>(h); }
}
