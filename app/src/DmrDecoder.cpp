#include "DmrDecoder.h"
#include "AmbeChip.h"
#include "golay2087_table.h"
#include <algorithm>
#include <cmath>
#include <cstring>

// DMR AMBE+2 de-interleave tables (from DSD include/dmr_const.h, ISC licence)
static const int rW[36] = {0,1,0,1,0,1, 0,1,0,1,0,1, 0,1,0,1,0,1, 0,1,0,1,0,2, 0,2,0,2,0,2, 0,2,0,2,0,2};
static const int rX[36] = {23,10,22,9,21,8, 20,7,19,6,18,5, 17,4,16,3,15,2, 14,1,13,0,12,10, 11,9,10,8,9,7, 8,6,7,5,6,4};
static const int rY[36] = {0,2,0,2,0,2, 0,2,0,3,0,3, 1,3,1,3,1,3, 1,3,1,3,1,3, 1,3,1,3,1,3, 1,3,1,3,1,3};
static const int rZ[36] = {5,3,4,2,3,1, 2,0,1,13,0,12, 22,11,21,10,20,9, 19,8,18,7,17,6, 16,5,15,4,14,3, 13,2,12,1,11,0};

static int golay23Parity(int data12);   // encoders section

static inline uint32_t be24(const char *p) {
    auto u = reinterpret_cast<const uint8_t *>(p);
    return (uint32_t(u[0]) << 16) | (uint32_t(u[1]) << 8) | u[2];
}

bool parseDmrd(const QByteArray &raw, DmrPacket &out)
{
    // "DMRD" seq(1) src(3) dst(3) rptr(4) flags(1) stream(4) data(33) [ber rssi]
    if (raw.size() < 53 || !raw.startsWith("DMRD"))
        return false;
    const char *d = raw.constData();
    const uint8_t flags = uint8_t(d[15]);
    out.seq = uint8_t(d[4]);
    out.src = be24(d + 5);
    out.dst = be24(d + 8);
    out.slot = (flags & 0x80) ? 2 : 1;
    out.privateCall = flags & 0x40;
    out.frameType = (flags >> 4) & 0x03;
    out.dataType = flags & 0x0F;
    out.stream = raw.mid(16, 4);
    out.payload = raw.mid(20, 33);
    return true;
}

static inline int bitAt(const QByteArray &p, int i)
{
    return (uint8_t(p[i >> 3]) >> (7 - (i & 7))) & 1;
}

QByteArray voiceBits(const QByteArray &payload33)
{
    // 108 voice bits | 48 sync/EMB bits | 108 voice bits
    QByteArray v(216, 0);
    for (int i = 0; i < 108; ++i) v[i] = char(bitAt(payload33, i));
    for (int i = 0; i < 108; ++i) v[108 + i] = char(bitAt(payload33, 156 + i));
    return v;
}

void AmbeDecoder::reset()
{
    mbe_initMbeParms(&cur_, &prev_, &prevEnh_);
}

// One 72-bit DMR AMBE+2 frame in on-air order (one bit per char) -> mbelib's ambe_fr layout.
void dmrDeinterleave(const char *fb, char ambe_fr[4][24])
{
    std::memset(ambe_fr, 0, 4 * 24);
    for (int i = 0; i < 36; ++i) {
        ambe_fr[rW[i]][rX[i]] = fb[2 * i];
        ambe_fr[rY[i]][rZ[i]] = fb[2 * i + 1];
    }
}

// With an AMBE chip (--ambe), the chip makes the audio from the 49 voice bits that mbelib's
// FEC recovered; the result replaces mbelib's. Its samples are scaled to mbelib's level.
static bool chipDecode2450(const char *ambe_d, float *out160, const void *owner)
{
    AmbeChip *chip = AmbeChip::instance();
    int16_t pcm[160];
    if (!chip || !chip->decode2450(ambe_d, pcm, owner)) return false;
    for (int i = 0; i < 160; ++i) out160[i] = pcm[i] / 16.0f;
    return true;
}

int AmbeDecoder::decodeBurst(const QByteArray &payload33, float *out480)
{
    const QByteArray v = voiceBits(payload33);
    int totalErrs = 0;
    for (int f = 0; f < 3; ++f) {
        char ambe_fr[4][24];
        std::memset(ambe_fr, 0, sizeof ambe_fr);
        const char *fb = v.constData() + f * 72;
        for (int i = 0; i < 36; ++i) {
            ambe_fr[rW[i]][rX[i]] = fb[2 * i];       // dibit high bit
            ambe_fr[rY[i]][rZ[i]] = fb[2 * i + 1];   // dibit low bit
        }
        char ambe_d[49];
        char err_str[64];
        int errs = 0, errs2 = 0;
        mbe_processAmbe3600x2450Framef(out480 + f * 160, &errs, &errs2, err_str,
                                       ambe_fr, ambe_d, &cur_, &prev_, &prevEnh_, 3);
        chipDecode2450(ambe_d, out480 + f * 160, this);
        totalErrs += errs2;
    }
    return totalErrs;
}

// ---------------------------------------------------------------- D-Star
// D-Star AMBE de-interleave tables (from DSD include/dstar_const.h, ISC licence).
// Bit i of the frame (on-air order: each byte LSB first) goes to ambe_fr[dW[i]][dX[i]].
static const int dW[72] = {0,0,3,2,1,1,0,0,1,1,0,0, 3,2,1,1,3,2,1,1,0,0,3,2,
                           0,0,3,2,1,1,0,0,1,1,0,0, 3,2,1,1,3,2,1,1,0,0,3,2,
                           0,0,3,2,1,1,0,0,1,1,0,0, 3,2,1,1,3,3,2,1,0,0,3,3};
static const int dX[72] = {10,22,11,9,10,22,11,23,8,20,9,21, 10,8,9,21,8,6,7,19,8,20,9,7,
                           6,18,7,5,6,18,7,19,4,16,5,17, 6,4,5,17,4,2,3,15,4,16,5,3,
                           2,14,3,1,2,14,3,15,0,12,1,13, 2,0,1,13,0,12,10,11,0,12,1,13};

static QString field(const char *p, int n)
{
    return QString::fromLatin1(p, n).trimmed();
}

bool parseDstr(const QByteArray &rec, DStarPacket &out)
{
    // rec = 'R'|'N' + "DSRP" type(1) id(2) seq(1) ...
    if (rec.size() < 1 + 8 || rec.mid(1, 4) != "DSRP") return false;
    const char *d = rec.constData() + 1;
    const int len = rec.size() - 1;
    const uint8_t type = uint8_t(d[4]);
    out.fromRf = rec[0] == 'R';
    out.id = uint16_t((uint8_t(d[5]) << 8) | uint8_t(d[6]));
    out.seq = uint8_t(d[7]) & 0x1F;
    out.isEnd = uint8_t(d[7]) & 0x40;
    if (type == 0x20) {
        // header: flags(3) RPT2(8) RPT1(8) YOUR(8) MY(8) MY-suffix(4) CRC(2) at offset 8
        if (len < 8 + 41) return false;
        const char *h = d + 8;
        out.isHeader = true;
        out.rpt2 = field(h + 3, 8);
        out.rpt1 = field(h + 11, 8);
        out.your = field(h + 19, 8);
        out.my = field(h + 27, 8);
        out.mySuffix = field(h + 35, 4);
        return true;
    }
    if (type == 0x21) {
        // voice: errors(1) AMBE(9) slow data(3) at offset 8
        if (len < 9 + 12) return false;
        out.isHeader = false;
        out.ambe = QByteArray(d + 9, 9);
        out.slowData = QByteArray(d + 18, 3);
        return true;
    }
    return false;
}

void DStarDecoder::reset()
{
    mbe_initMbeParms(&cur_, &prev_, &prevEnh_);
}

int DStarDecoder::decodeFrame(const QByteArray &ambe9, float *out160)
{
    char ambe_fr[4][24];
    std::memset(ambe_fr, 0, sizeof ambe_fr);
    for (int i = 0; i < 72; ++i)
        ambe_fr[dW[i]][dX[i]] = char((uint8_t(ambe9[i >> 3]) >> (i & 7)) & 1);
    char ambe_d[49];
    char err_str[64];
    int errs = 0, errs2 = 0;
    mbe_processAmbe3600x2400Framef(out160, &errs, &errs2, err_str, ambe_fr, ambe_d,
                                   &cur_, &prev_, &prevEnh_, 3);
    if (AmbeChip *chip = AmbeChip::instance()) {   // the chip decodes the frame as received
        int16_t pcm[160];
        if (chip->decodeDStar(ambe9, pcm, this))
            for (int i = 0; i < 160; ++i) out160[i] = pcm[i] / 16.0f;
    }
    return errs2;
}

void SlowDataText::reset()
{
    haveHalf_ = false;
    std::memset(chars_, ' ', sizeof chars_);
    std::memset(got_, 0, sizeof got_);
}

void SlowDataText::feed(uint8_t seq, const QByteArray &slow3)
{
    static const uint8_t scramble[3] = {0x70, 0x4F, 0x93};
    if (seq == 0 || slow3.size() < 3) {   // frame 0 carries the sync pattern
        haveHalf_ = false;
        return;
    }
    uint8_t b[3];
    for (int i = 0; i < 3; ++i) b[i] = uint8_t(slow3[i]) ^ scramble[i];
    if (seq & 1) {                        // first half of a 6-byte block
        std::memcpy(half_, b, 3);
        haveHalf_ = true;
        return;
    }
    if (!haveHalf_) return;
    haveHalf_ = false;
    const uint8_t block[6] = {half_[0], half_[1], half_[2], b[0], b[1], b[2]};
    if ((block[0] & 0xF0) != 0x40) return;   // 0x4n = text message block n
    const int n = block[0] & 0x0F;
    if (n > 3) return;
    for (int i = 0; i < 5; ++i) {
        const char c = char(block[1 + i]);
        chars_[n * 5 + i] = (c >= 0x20 && c < 0x7F) ? c : ' ';
    }
    got_[n] = true;
}

QString SlowDataText::text() const
{
    if (!(got_[0] || got_[1] || got_[2] || got_[3])) return {};
    return QString::fromLatin1(chars_, 20).trimmed();
}

// ---------------------------------------------------------------- YSF
// Tables from MMDVMHost YSFFICH.cpp / YSFPayload.cpp (GPL v2).
static const unsigned FICH_INTERLEAVE[100] = {
    0, 40, 80, 120, 160,  2, 42, 82, 122, 162,  4, 44, 84, 124, 164,  6, 46, 86, 126, 166,
    8, 48, 88, 128, 168, 10, 50, 90, 130, 170, 12, 52, 92, 132, 172, 14, 54, 94, 134, 174,
    16, 56, 96, 136, 176, 18, 58, 98, 138, 178, 20, 60, 100, 140, 180, 22, 62, 102, 142, 182,
    24, 64, 104, 144, 184, 26, 66, 106, 146, 186, 28, 68, 108, 148, 188, 30, 70, 110, 150, 190,
    32, 72, 112, 152, 192, 34, 74, 114, 154, 194, 36, 76, 116, 156, 196, 38, 78, 118, 158, 198};
static const unsigned VCH_INTERLEAVE[104] = {
    0, 4, 8, 12, 16, 20, 24, 28, 32, 36, 40, 44, 48, 52, 56, 60, 64, 68, 72, 76, 80, 84, 88, 92, 96, 100,
    1, 5, 9, 13, 17, 21, 25, 29, 33, 37, 41, 45, 49, 53, 57, 61, 65, 69, 73, 77, 81, 85, 89, 93, 97, 101,
    2, 6, 10, 14, 18, 22, 26, 30, 34, 38, 42, 46, 50, 54, 58, 62, 66, 70, 74, 78, 82, 86, 90, 94, 98, 102,
    3, 7, 11, 15, 19, 23, 27, 31, 35, 39, 43, 47, 51, 55, 59, 63, 67, 71, 75, 79, 83, 87, 91, 95, 99, 103};
static const uint8_t VCH_WHITENING[13] = {0x93, 0xD7, 0x51, 0x21, 0x9C, 0x2F, 0x6C, 0xD0, 0xEF, 0x0F, 0xF8, 0x3D, 0xF1};

static inline int rbit(const uint8_t *p, unsigned i) { return (p[i >> 3] >> (7 - (i & 7))) & 1; }
static inline void wbit(uint8_t *p, unsigned i, int b)
{
    if (b) p[i >> 3] |= uint8_t(0x80 >> (i & 7)); else p[i >> 3] &= uint8_t(~(0x80 >> (i & 7)));
}

// CRC-16/CCITT (poly 0x1021, init 0, result inverted), as MMDVMHost checkCCITT162.
static bool checkCcitt162(const uint8_t *in, int len)
{
    uint16_t crc = 0;
    for (int i = 0; i < len - 2; ++i) {
        crc ^= uint16_t(in[i]) << 8;
        for (int b = 0; b < 8; ++b) crc = (crc & 0x8000) ? uint16_t((crc << 1) ^ 0x1021) : uint16_t(crc << 1);
    }
    crc = uint16_t(~crc);
    return uint8_t(crc >> 8) == in[len - 2] && uint8_t(crc) == in[len - 1];
}

// Network frames are regenerated by MMDVMHost/YSFGateway, so the FICH arrives
// error-free: invert the rate-1/2 convolutional code directly instead of
// running a Viterbi decoder, take the data half of each Golay(24,12) word,
// and let the CRC reject anything that isn't clean.
static bool decodeFich(const uint8_t *frame, uint8_t fich[6])
{
    const uint8_t *b = frame + 5;              // skip sync
    uint8_t out[13] = {};
    int d1 = 0, d2 = 0, d3 = 0, d4 = 0;
    for (unsigned i = 0; i < 100; ++i) {
        const unsigned n = FICH_INTERLEAVE[i];
        const int g1 = rbit(b, n);             // g1 = d ^ d3 ^ d4
        const int d = g1 ^ d3 ^ d4;
        if (i < 96) wbit(out, i, d);
        d4 = d3; d3 = d2; d2 = d1; d1 = d;
    }
    uint32_t w[4];
    for (int k = 0; k < 4; ++k)
        w[k] = ((uint32_t(out[3 * k]) << 16) | (uint32_t(out[3 * k + 1]) << 8) | out[3 * k + 2]) >> 12;
    fich[0] = uint8_t(w[0] >> 4);
    fich[1] = uint8_t(((w[0] << 4) & 0xF0) | ((w[1] >> 8) & 0x0F));
    fich[2] = uint8_t(w[1]);
    fich[3] = uint8_t(w[2] >> 4);
    fich[4] = uint8_t(((w[2] << 4) & 0xF0) | ((w[3] >> 8) & 0x0F));
    fich[5] = uint8_t(w[3]);
    return checkCcitt162(fich, 6);
}

bool parseYsf(const QByteArray &rec, YsfPacket &out)
{
    if (rec.size() < 1 + 155 || rec.mid(1, 4) != "YSFD") return false;
    const char *d = rec.constData() + 1;
    out.fromRf = rec[0] == 'R';
    auto cs = [d](int off) { return QString::fromLatin1(d + off, 10).replace(QChar(0), ' ').trimmed(); };
    out.gateway = cs(4);
    out.src = cs(14);
    out.dst = cs(24);
    out.end = uint8_t(d[34]) & 0x01;
    out.count = (uint8_t(d[34]) >> 1) & 0x7F;
    out.frame = QByteArray(d + 35, 120);
    uint8_t fich[6];
    out.fichOk = decodeFich(reinterpret_cast<const uint8_t *>(out.frame.constData()), fich);
    if (out.fichOk) {
        out.fi = (fich[0] >> 6) & 0x03;
        out.fn = (fich[1] >> 3) & 0x07;
        out.ft = fich[1] & 0x07;
        out.dt = fich[2] & 0x03;
        out.dgid = fich[3] & 0x7F;
    }
    return true;
}

void YsfDecoder::reset()
{
    mbe_initMbeParms(&cur_, &prev_, &prevEnh_);
}

QByteArray ysfEncodeFrame(int fi, int fn, int ft, int dt, const uint8_t (*ambe49)[49])
{
    uint8_t f[120] = {0xD4, 0x71, 0xC9, 0x63, 0x4D};
    // FICH: FI CS CM BN | BT FN FT | ... DT | DGID, then CRC-CCITT (init 0, inverted)
    uint8_t fich[6] = {uint8_t((fi & 3) << 6), uint8_t(((fn & 7) << 3) | (ft & 7)), uint8_t(dt & 3), 0, 0, 0};
    uint16_t crc = 0;
    for (int i = 0; i < 4; ++i) {
        crc ^= uint16_t(fich[i]) << 8;
        for (int b = 0; b < 8; ++b) crc = (crc & 0x8000) ? uint16_t((crc << 1) ^ 0x1021) : uint16_t(crc << 1);
    }
    crc = uint16_t(~crc);
    fich[4] = uint8_t(crc >> 8);
    fich[5] = uint8_t(crc);
    const int w[4] = {(fich[0] << 4) | (fich[1] >> 4), ((fich[1] & 0x0F) << 8) | fich[2],
                      (fich[3] << 4) | (fich[4] >> 4), ((fich[4] & 0x0F) << 8) | fich[5]};
    uint8_t bits[100] = {};
    for (int k = 0; k < 4; ++k) {
        const int code = (w[k] << 11) | golay23Parity(w[k]);
        int ones = 0;
        for (int i = 0; i < 23; ++i) { bits[24 * k + i] = uint8_t((code >> (22 - i)) & 1); ones += bits[24 * k + i]; }
        bits[24 * k + 23] = uint8_t(ones & 1);
    }
    // rate 1/2, K=5 convolutional code: g1 = d^d3^d4, g2 = d^d1^d2^d4
    int d1 = 0, d2 = 0, d3 = 0, d4 = 0;
    for (unsigned i = 0; i < 100; ++i) {
        const int d = bits[i];
        const unsigned n = FICH_INTERLEAVE[i];
        wbit(f + 5, n, d ^ d3 ^ d4);
        wbit(f + 5, n + 1, d ^ d1 ^ d2 ^ d4);
        d4 = d3; d3 = d2; d2 = d1; d1 = d;
    }
    if (ambe49) {
        uint8_t *data = f + 30;
        for (int j = 0; j < 5; ++j) {
            uint8_t vch[13] = {};
            for (int k = 0; k < 27; ++k)
                for (int r = 0; r < 3; ++r) wbit(vch, 3 * k + r, ambe49[j][k]);
            for (int k = 0; k < 22; ++k) wbit(vch, 81 + k, ambe49[j][27 + k]);
            for (int i = 0; i < 13; ++i) vch[i] ^= VCH_WHITENING[i];
            const unsigned offset = 40 + 144 * j;
            for (unsigned i = 0; i < 104; ++i) wbit(data, offset + VCH_INTERLEAVE[i], rbit(vch, i));
        }
    }
    return QByteArray(reinterpret_cast<const char *>(f), 120);
}

int YsfDecoder::decodeVD2(const QByteArray &frame120, float *out800)
{
    const uint8_t *data = reinterpret_cast<const uint8_t *>(frame120.constData()) + 5 + 25;
    int voteErrors = 0;
    unsigned offset = 40;                      // first VCH follows DCH(0)
    for (int j = 0; j < 5; ++j, offset += 144) {
        uint8_t vch[13] = {};
        for (unsigned i = 0; i < 104; ++i) wbit(vch, i, rbit(data, offset + VCH_INTERLEAVE[i]));
        for (int i = 0; i < 13; ++i) vch[i] ^= VCH_WHITENING[i];

        // 27 bits sent three times (majority vote), then 22 bits sent once.
        char ambe_d[49];
        for (int k = 0; k < 27; ++k) {
            const int v = rbit(vch, 3 * k) + rbit(vch, 3 * k + 1) + rbit(vch, 3 * k + 2);
            if (v == 1 || v == 2) ++voteErrors;
            ambe_d[k] = char(v >= 2);
        }
        for (int k = 0; k < 22; ++k) ambe_d[27 + k] = char(rbit(vch, 81 + k));

        int errs = 0, errs2 = 0;
        char err_str[64];
        mbe_processAmbe2450Dataf(out800 + j * 160, &errs, &errs2, err_str, ambe_d,
                                 &cur_, &prev_, &prevEnh_, 3);
        chipDecode2450(ambe_d, out800 + j * 160, this);
    }
    return voteErrors;
}

// ---------------------------------------------------------------- P25
void P25Decoder::reset()
{
    mbe_initMbeParms(&cur_, &prev_, &prevEnh_);
}

void P25Decoder::decodeImbe(const char *imbe11, float *out160)
{
    char imbe_d[88];
    for (int i = 0; i < 88; ++i) imbe_d[i] = char((uint8_t(imbe11[i >> 3]) >> (7 - (i & 7))) & 1);
    int errs = 0, errs2 = 0;
    char err_str[64];
    mbe_processImbe4400Dataf(out160, &errs, &errs2, err_str, imbe_d, &cur_, &prev_, &prevEnh_, 3);
}

// ---------------------------------------------------------------- encoders (test calls)
static const int kGolayGenerator[12] = {   // as mbelib ecc_const.h
    0x63a, 0x31d, 0x7b4, 0x3da, 0x1ed, 0x6cc, 0x366, 0x1b3, 0x6e3, 0x54b, 0x49f, 0x475};

static int golay23Parity(int data12)
{
    int p = 0;
    for (int i = 0; i < 12; ++i)
        if (data12 & (0x800 >> i)) p ^= kGolayGenerator[i];
    return p;
}

void ambe49ToFrame(const uint8_t d[49], char fr[4][24])
{
    std::memset(fr, 0, 4 * 24);
    int a = 0, b = 0;
    for (int i = 0; i < 12; ++i) { a = (a << 1) | d[i]; b = (b << 1) | d[12 + i]; }
    // C0: data in fr[0][23..12], Golay(23,12) parity in fr[0][11..1], overall parity in fr[0][0]
    const int pa = golay23Parity(a), pb = golay23Parity(b);
    int ones = 0;
    for (int i = 0; i < 12; ++i) { fr[0][23 - i] = char(d[i]); ones += d[i]; }
    for (int k = 0; k < 11; ++k) { fr[0][11 - k] = char((pa >> (10 - k)) & 1); ones += fr[0][11 - k]; }
    fr[0][0] = char(ones & 1);
    // C1: data in fr[1][22..11], parity in fr[1][10..0], then scrambled by the PRNG seeded from C0
    for (int i = 0; i < 12; ++i) fr[1][22 - i] = char(d[12 + i]);
    for (int k = 0; k < 11; ++k) fr[1][10 - k] = char((pb >> (10 - k)) & 1);
    for (int i = 0; i < 11; ++i) fr[2][10 - i] = char(d[24 + i]);
    for (int i = 0; i < 14; ++i) fr[3][13 - i] = char(d[35 + i]);
    unsigned short pr[24];
    pr[0] = uint16_t(16 * a);
    for (int i = 1; i < 24; ++i)
        pr[i] = uint16_t((173 * pr[i - 1]) + 13849 - (65536 * (((173 * pr[i - 1]) + 13849) / 65536)));
    for (int i = 1; i < 24; ++i) pr[i] = pr[i] / 32768;
    int k = 1;
    for (int j = 22; j >= 0; --j) fr[1][j] = char(fr[1][j] ^ pr[k++]);
}

// Reed-Solomon (12,9) over GF(256), poly 0x11D, generator {64, 56, 14, 1} (as MMDVMHost RS129).
static uint8_t gfMul(uint8_t a, uint8_t b)
{
    static uint8_t expT[512], logT[256];
    static bool init = false;
    if (!init) {
        int x = 1;
        for (int i = 0; i < 255; ++i) {
            expT[i] = uint8_t(x);
            logT[x] = uint8_t(i);
            x <<= 1;
            if (x & 0x100) x ^= 0x11D;
        }
        for (int i = 255; i < 512; ++i) expT[i] = expT[i - 255];
        init = true;
    }
    return (a && b) ? expT[logT[a] + logT[b]] : 0;
}

static void rs129Parity(const uint8_t msg[9], uint8_t parity[3])
{
    static const uint8_t poly[3] = {64, 56, 14};
    uint8_t p[3] = {0, 0, 0};
    for (int i = 0; i < 9; ++i) {
        const uint8_t d = msg[i] ^ p[2];
        p[2] = p[1] ^ gfMul(poly[2], d);
        p[1] = p[0] ^ gfMul(poly[1], d);
        p[0] = gfMul(poly[0], d);
    }
    parity[0] = p[0]; parity[1] = p[1]; parity[2] = p[2];
}

QByteArray dmrEncodeLcBurst(const uint8_t lc9[9], int dataType, int cc)
{
    uint8_t lc[12];
    std::memcpy(lc, lc9, 9);
    uint8_t par[3];
    rs129Parity(lc, par);
    const uint8_t mask = dataType == 1 ? 0x96 : 0x99;
    lc[9] = par[2] ^ mask;
    lc[10] = par[1] ^ mask;
    lc[11] = par[0] ^ mask;

    // BPTC(196,96): place data, Hamming(15,11) rows, Hamming(13,9) columns, interleave
    bool d[196] = {}, raw[196] = {};
    bool bits[96];
    for (int i = 0; i < 96; ++i) bits[i] = (lc[i >> 3] >> (7 - (i & 7))) & 1;
    int pos = 0;
    const int ranges[9][2] = {{4, 11}, {16, 26}, {31, 41}, {46, 56}, {61, 71}, {76, 86}, {91, 101}, {106, 116}, {121, 131}};
    for (auto &r : ranges)
        for (int a = r[0]; a <= r[1]; ++a) d[a] = bits[pos++];
    for (int r = 0; r < 9; ++r) {
        bool *x = d + r * 15 + 1;
        x[11] = x[0] ^ x[1] ^ x[2] ^ x[3] ^ x[5] ^ x[7] ^ x[8];
        x[12] = x[1] ^ x[2] ^ x[3] ^ x[4] ^ x[6] ^ x[8] ^ x[9];
        x[13] = x[2] ^ x[3] ^ x[4] ^ x[5] ^ x[7] ^ x[9] ^ x[10];
        x[14] = x[0] ^ x[1] ^ x[2] ^ x[4] ^ x[6] ^ x[7] ^ x[10];
    }
    for (int c = 0; c < 15; ++c) {
        bool col[13];
        for (int a = 0; a < 13; ++a) col[a] = d[c + 1 + a * 15];
        col[9] = col[0] ^ col[1] ^ col[3] ^ col[5] ^ col[6];
        col[10] = col[0] ^ col[1] ^ col[2] ^ col[4] ^ col[6] ^ col[7];
        col[11] = col[0] ^ col[1] ^ col[2] ^ col[3] ^ col[5] ^ col[7] ^ col[8];
        col[12] = col[0] ^ col[2] ^ col[4] ^ col[5] ^ col[8];
        for (int a = 0; a < 13; ++a) d[c + 1 + a * 15] = col[a];
    }
    for (int a = 0; a < 196; ++a) raw[(a * 181) % 196] = d[a];

    uint8_t out[33] = {};
    auto byteAt = [&raw](int start) {
        uint8_t b = 0;
        for (int i = 0; i < 8; ++i) b = uint8_t((b << 1) | (raw[start + i] ? 1 : 0));
        return b;
    };
    for (int i = 0; i < 12; ++i) out[i] = byteAt(i * 8);
    const uint8_t two = byteAt(96);
    out[12] = uint8_t((out[12] & 0x3F) | (two & 0xC0));
    out[20] = uint8_t((out[20] & 0xFC) | ((two >> 4) & 0x03));
    for (int i = 0; i < 12; ++i) out[21 + i] = byteAt(100 + i * 8);

    // slot type: colour code + data type, Golay(20,8)
    uint8_t st[3];
    st[0] = uint8_t(((cc << 4) & 0xF0) | (dataType & 0x0F));
    const unsigned ck = kGolay2087Table[st[0]];
    st[1] = uint8_t(ck & 0xFF);
    st[2] = uint8_t(ck >> 8);
    out[12] = uint8_t((out[12] & 0xC0) | ((st[0] >> 2) & 0x3F));
    out[13] = uint8_t((out[13] & 0x0F) | ((st[0] << 6) & 0xC0) | ((st[1] >> 2) & 0x30));
    out[19] = uint8_t((out[19] & 0xF0) | ((st[1] >> 2) & 0x0F));
    out[20] = uint8_t((out[20] & 0x03) | ((st[1] << 6) & 0xC0) | ((st[2] >> 2) & 0x3C));

    // BS-sourced data sync
    static const uint8_t sync[7] = {0x0D, 0xFF, 0x57, 0xD7, 0x5D, 0xF5, 0xD0};
    static const uint8_t smask[7] = {0x0F, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xF0};
    for (int i = 0; i < 7; ++i) out[13 + i] = uint8_t((out[13 + i] & ~smask[i]) | sync[i]);
    return QByteArray(reinterpret_cast<const char *>(out), 33);
}

QByteArray dmrEncodeBurst(const uint8_t ambe49[3][49], const uint8_t mid48[6])
{
    char v[216];
    for (int f = 0; f < 3; ++f) {
        char fr[4][24];
        ambe49ToFrame(ambe49[f], fr);
        for (int i = 0; i < 36; ++i) {
            v[f * 72 + 2 * i] = fr[rW[i]][rX[i]];
            v[f * 72 + 2 * i + 1] = fr[rY[i]][rZ[i]];
        }
    }
    QByteArray out(33, 0);
    auto put = [&out](int i, int b) { if (b) out[i >> 3] = char(uint8_t(out[i >> 3]) | (0x80 >> (i & 7))); };
    for (int i = 0; i < 108; ++i) put(i, v[i]);
    for (int i = 0; i < 48; ++i) put(108 + i, (mid48[i >> 3] >> (7 - (i & 7))) & 1);
    for (int i = 0; i < 108; ++i) put(156 + i, v[108 + i]);
    return out;
}

// ---------------------------------------------------------------- NXDN
bool parseNxdn(const QByteArray &rec, NxdnPacket &out)
{
    // rec = 'R'|'N' + 102-byte "ICOM" packet
    if (rec.size() < 1 + 102 || rec.mid(1, 4) != "ICOM") return false;
    out.fromRf = rec[0] == 'R';
    out.data = rec.mid(1 + 40, 33);
    const uint8_t lich = uint8_t(out.data[0]);
    out.usc = (lich >> 4) & 3;
    out.option = (lich >> 2) & 3;
    return true;
}

void NxdnDecoder::reset()
{
    mbe_initMbeParms(&cur_, &prev_, &prevEnh_);
}

void NxdnDecoder::decodeHalf(const uint8_t *half14, float *out320)
{
    for (int f = 0; f < 2; ++f) {
        char ambe_d[49];
        for (int k = 0; k < 49; ++k) {
            const int b = f * 49 + k;
            ambe_d[k] = char((half14[b >> 3] >> (7 - (b & 7))) & 1);
        }
        int errs = 0, errs2 = 0;
        char err_str[64];
        mbe_processAmbe2450Dataf(out320 + f * 160, &errs, &errs2, err_str, ambe_d,
                                 &cur_, &prev_, &prevEnh_, 3);
        chipDecode2450(ambe_d, out320 + f * 160, this);
    }
}

// ---------------------------------------------------------------- M17
QString m17Callsign(const uint8_t *e)
{
    uint64_t enc = 0;
    for (int i = 0; i < 6; ++i) enc = (enc << 8) | e[i];
    if (enc == 0xFFFFFFFFFFFFULL) return "ALL";
    if (enc >= 268697600000000ULL) return "(invalid)";
    QString call;
    if (enc >= 262144000000000ULL) { call = "#"; enc -= 262144000000000ULL; }
    static const char *alpha = " ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-/.";
    while (enc) { call += QChar(alpha[enc % 40]); enc /= 40; }
    return call.simplified();
}

bool parseM17(const QByteArray &rec, M17Packet &out)
{
    // rec = 'R'|'N' + 54-byte "M17 " frame
    if (rec.size() < 1 + 54 || rec.mid(1, 4) != "M17 ") return false;
    const auto *d = reinterpret_cast<const uint8_t *>(rec.constData() + 1);
    out.fromRf = rec[0] == 'R';
    out.sid = uint16_t((d[4] << 8) | d[5]);
    out.dst = m17Callsign(d + 6);
    out.src = m17Callsign(d + 12);
    const uint8_t type = d[19];
    out.dataType = (type >> 1) & 3;
    out.encType = (type >> 3) & 3;
    out.end = (d[34] & 0x80) != 0;
    out.payload = QByteArray(reinterpret_cast<const char *>(d + 36), 16);
    return true;
}

#ifdef HAVE_CODEC2
#include <codec2/codec2.h>
M17Decoder::M17Decoder()
{
    c3200_ = codec2_create(CODEC2_MODE_3200);
    c1600_ = codec2_create(CODEC2_MODE_1600);
}
M17Decoder::~M17Decoder()
{
    if (c3200_) codec2_destroy(static_cast<CODEC2 *>(c3200_));
    if (c1600_) codec2_destroy(static_cast<CODEC2 *>(c1600_));
}
bool M17Decoder::available() { return true; }
void M17Decoder::reset() {}
int M17Decoder::decode(const M17Packet &p, float *out320)
{
    short pcm[320];
    const auto *bits = reinterpret_cast<const unsigned char *>(p.payload.constData());
    if (p.dataType == 2) {
        codec2_decode(static_cast<CODEC2 *>(c3200_), pcm, bits);         // 2 x 20 ms
        codec2_decode(static_cast<CODEC2 *>(c3200_), pcm + 160, bits + 8);
    } else if (p.dataType == 3) {
        codec2_decode(static_cast<CODEC2 *>(c1600_), pcm, bits);         // 40 ms
    } else {
        return 0;
    }
    for (int i = 0; i < 320; ++i) out320[i] = pcm[i] / 8.0f;   // roughly mbelib's level for the AGC
    return 320;
}
#else
M17Decoder::M17Decoder() = default;
M17Decoder::~M17Decoder() = default;
bool M17Decoder::available() { return false; }
void M17Decoder::reset() {}
int M17Decoder::decode(const M17Packet &, float *) { return 0; }
#endif

void NoiseGate::reset()
{
    recent_.clear();
    holdLeft_ = 0;
    gain_ = 1.0;
}

void NoiseGate::process(float *pcm, int n, const Params &p)
{
    const double closed = std::pow(10.0, -p.reductionDb / 20.0);
    const double ratio = std::pow(10.0, p.thresholdDb / 20.0);
    for (int start = 0; start < n; start += 160) {
        const int len = std::min(160, n - start);
        float *b = pcm + start;
        double acc = 0;
        for (int i = 0; i < len; ++i) acc += double(b[i]) * b[i];
        const double rms = std::sqrt(acc / std::max(1, len)) + 1e-3;

        // Background = the 10th percentile of block levels over the last 1.5 s. Speech always has
        // gaps between syllables, so this follows the noise rather than the voice, and a single
        // odd quiet block (e.g. at key-up) doesn't drag it down. Digital silence is left out.
        if (rms > 8) {
            recent_.push_back(rms);
            if (recent_.size() > 75) recent_.pop_front();
        }
        double floor = rms;
        if (recent_.size() >= 10) {
            std::vector<double> v(recent_.begin(), recent_.end());
            std::nth_element(v.begin(), v.begin() + long(v.size() / 10), v.end());
            floor = v[v.size() / 10];
        } else if (!recent_.empty()) {
            floor = *std::min_element(recent_.begin(), recent_.end());
        }

        if (rms > floor * ratio) holdLeft_ = p.holdMs * 8;   // 8 samples per ms
        const double target = holdLeft_ > 0 ? 1.0 : closed;
        holdLeft_ = std::max(0, holdLeft_ - len);

        // Open within one block (5 ms ramp), close more gently (about 60 ms), sample by sample.
        const double step = target > gain_ ? 1.0 / 40 : 1.0 / 480;
        for (int i = 0; i < len; ++i) {
            if (gain_ < target) gain_ = std::min(target, gain_ + step);
            else if (gain_ > target) gain_ = std::max(target, gain_ - step * std::max(gain_, 0.05));
            b[i] = float(b[i] * gain_);
        }
    }
}

void Agc::process(const float *in, int n, double volume, std::vector<int16_t> &out, double &rmsOut)
{
    float peak = 0.f;
    for (int i = 0; i < n; ++i) peak = std::max(peak, std::fabs(in[i]));
    if (peak > 0.f) {
        const double want = std::min(60.0, 20000.0 / peak);
        gain_ = (want < gain_) ? want : gain_ + (want - gain_) * 0.05;
    }
    out.resize(n);
    double acc = 0;
    for (int i = 0; i < n; ++i) {
        double s = in[i] * gain_;
        acc += s * s;
        s *= volume;
        out[i] = int16_t(std::clamp(s, -32767.0, 32767.0));
    }
    rmsOut = std::sqrt(acc / std::max(1, n));   // pre-volume, for the level meter
}
