// --ambe SPEC --ambe-test file.wav : check an AMBE chip against mbelib and the OP25 encoders.
// The speech in the WAV (8 kHz, 16-bit mono, e.g. a call recording) is encoded and decoded in
// every combination; each line prints how closely the result matches the original (best
// band-energy correlation over a few frames of delay: ~0.8+ = good speech, ~0.3 = garbage).
#include "AmbeChip.h"
#include "DmrDecoder.h"
#include "../third_party/op25_vocoder/op25voc.h"
#include <QElapsedTimer>
#include <QFile>
#include <array>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <vector>

namespace {
std::vector<int16_t> readWav(const QString &path)
{
    QFile f(path);
    std::vector<int16_t> pcm;
    if (!f.open(QIODevice::ReadOnly)) return pcm;
    const QByteArray d = f.readAll();
    int pos = 12;
    while (pos + 8 <= d.size()) {
        const QByteArray id = d.mid(pos, 4);
        const int len = int(uint8_t(d[pos + 4]) | uint8_t(d[pos + 5]) << 8 | uint8_t(d[pos + 6]) << 16 | uint8_t(d[pos + 7]) << 24);
        if (id == "data") {
            for (int i = pos + 8; i + 1 < pos + 8 + len && i + 1 < d.size(); i += 2)
                pcm.push_back(int16_t(uint8_t(d[i]) | (uint8_t(d[i + 1]) << 8)));
            break;
        }
        pos += 8 + len + (len & 1);
    }
    return pcm;
}

// Speech-envelope match: vocoders rebuild speech with their own phase, so compare what a
// listener hears instead of the waveform: the log energy in 8 bands (0-4 kHz) of each 20 ms
// frame, correlated over time (allowing a few frames of codec delay).
std::vector<std::array<double, 8>> bands(const std::vector<float> &x)
{
    std::vector<std::array<double, 8>> out;
    for (size_t f = 0; f + 160 <= x.size(); f += 160) {
        std::array<double, 8> e{};
        for (int k = 1; k < 80; ++k) {   // 50 Hz bins
            double re = 0, im = 0;
            for (int n = 0; n < 160; ++n) {
                const double w = 0.5 - 0.5 * std::cos(2 * M_PI * n / 159.0);
                re += x[f + n] * w * std::cos(2 * M_PI * k * n / 160.0);
                im -= x[f + n] * w * std::sin(2 * M_PI * k * n / 160.0);
            }
            e[size_t(k / 10)] += re * re + im * im;
        }
        for (double &v : e) v = std::log10(v + 1.0);
        out.push_back(e);
    }
    return out;
}

double match(const std::vector<float> &a, const std::vector<float> &b)
{
    const auto A = bands(a), B = bands(b);
    double best = 0;
    for (int lag = 0; lag <= 6; ++lag) {
        std::vector<double> x, y;
        for (size_t i = 0; i + lag < B.size() && i < A.size(); ++i)
            for (int k = 0; k < 8; ++k) { x.push_back(A[i][k]); y.push_back(B[i + lag][k]); }
        if (x.size() < 16) continue;
        double mx = 0, my = 0;
        for (size_t i = 0; i < x.size(); ++i) { mx += x[i]; my += y[i]; }
        mx /= double(x.size());
        my /= double(y.size());
        double xy = 0, xx = 0, yy = 0;
        for (size_t i = 0; i < x.size(); ++i) {
            xy += (x[i] - mx) * (y[i] - my);
            xx += (x[i] - mx) * (x[i] - mx);
            yy += (y[i] - my) * (y[i] - my);
        }
        if (xx > 0 && yy > 0) best = std::max(best, xy / std::sqrt(xx * yy));
    }
    return best;
}
}

int ambeSelfTest(const QString &spec, const QString &wav)
{
    QString err;
    AmbeChip *chip = AmbeChip::open(spec, &err);
    if (!chip) { fprintf(stdout, "AMBE chip: %s\n", qPrintable(err)); return 1; }
    fprintf(stdout, "AMBE chip: %s, version %s, speech %s\n", qPrintable(chip->productId()), qPrintable(chip->version()),
            chip->companded() ? "8-bit mu-law" : "16-bit");
    const std::vector<int16_t> in = readWav(wav);
    const size_t frames = std::min<size_t>(in.size() / 160, 250);
    if (frames < 10) { fprintf(stdout, "%s: no 8 kHz 16-bit audio found\n", qPrintable(wav)); return 1; }
    std::vector<float> ref(in.begin(), in.begin() + long(frames * 160));
    fprintf(stdout, "%zu frames (%.1f s) of %s\n", frames, frames * 0.02, qPrintable(wav));

    bool allOk = true;
    auto report = [&](const char *name, const std::vector<float> &out, bool ok) {
        const double m = ok ? match(ref, out) : 0;
        allOk = allOk && ok && m > 0.7;
        fprintf(stdout, "  %-44s %s  match %.2f\n", name, ok ? "ok  " : "FAIL", m);
        fflush(stdout);
    };

    // Each combination runs as its own pass: AMBE decoding carries state from frame to frame,
    // so one decoder must never see two streams interleaved.
    auto toChars = [](const uint8_t *b, char *d, int n) { for (int i = 0; i < n; ++i) d[i] = char(b[i]); };
    auto packDStar = [](const uint8_t *bits) {
        QByteArray a(9, 0);
        for (int i = 0; i < 72; ++i)
            if (bits[i]) a[i >> 3] = char(uint8_t(a[i >> 3]) | (1 << (i & 7)));
        return a;
    };
    for (int mode = 0; mode < 2; ++mode) {   // 0 = AMBE+2 2450 (DMR/YSF/NXDN), 1 = D-Star
        const char *label = mode == 0 ? "2450 (DMR/YSF/NXDN)" : "D-Star";
        for (int pass = 0; pass < 4; ++pass) {   // chip->chip, chip->mbelib, OP25->chip, OP25->mbelib
            std::vector<float> out;
            bool ok = true;
            mbe_parms cur{}, prev{}, enh{};
            mbe_initMbeParms(&cur, &prev, &enh);
            DStarDecoder soft;
            void *op25 = op25voc_ambe_new(mode);
            chip->rawRate(mode == 0 ? QByteArray::fromHex("0922") : QByteArray::fromHex("0a01300763400000000000000048"));
            for (size_t f = 0; f < frames && ok; ++f) {
                const int16_t *p = in.data() + f * 160;
                const bool chipEnc = pass == 0 || pass == 1, chipDec = pass == 0 || pass == 2;
                int16_t pcm[160];
                float fl[160];
                if (mode == 0) {
                    uint8_t b[49];
                    char d[49];
                    if (chipEnc) ok = chip->encode2450(p, b); else op25voc_ambe_encode(op25, p, b);
                    toChars(b, d, 49);
                    if (chipDec) {
                        ok = ok && chip->decode2450(d, pcm);
                        out.insert(out.end(), pcm, pcm + 160);
                    } else {
                        int x1 = 0, x2 = 0;
                        char es[64];
                        mbe_processAmbe2450Dataf(fl, &x1, &x2, es, d, &cur, &prev, &enh, 3);
                        out.insert(out.end(), fl, fl + 160);
                    }
                } else {
                    QByteArray a;
                    if (chipEnc) ok = chip->encodeDStar(p, a);
                    else { uint8_t b[72]; op25voc_ambe_encode(op25, p, b); a = packDStar(b); }
                    if (chipDec) {
                        ok = ok && chip->decodeDStar(a, pcm);
                        out.insert(out.end(), pcm, pcm + 160);
                    } else {
                        AmbeChip::setBypass(true);
                        soft.decodeFrame(a, fl);
                        AmbeChip::setBypass(false);
                        out.insert(out.end(), fl, fl + 160);
                    }
                }
            }
            op25voc_ambe_free(op25);
            static const char *names[] = {"chip -> chip", "chip -> mbelib", "OP25 encoder -> chip", "OP25 -> mbelib (software)"};
            report(qPrintable(QString("%1: %2").arg(label, names[pass])), out, ok);
        }
    }
    if (qEnvironmentVariableIsSet("DT_AMBE_TIMING")) {   // how long one 20 ms frame takes on the chip
        QElapsedTimer t;
        uint8_t b[49];
        int16_t pcm[160];
        chip->rawRate(QByteArray::fromHex("0922"));
        t.start();
        for (int i = 0; i < 100; ++i) chip->encode2450(in.data() + (i % frames) * 160, b);
        const double enc = t.nsecsElapsed() / 1e6 / 100;
        char d[49] = {};
        t.restart();
        for (int i = 0; i < 100; ++i) chip->decode2450(d, pcm);
        const double dec = t.nsecsElapsed() / 1e6 / 100;
        fprintf(stdout, "  timing per 20 ms frame: encode %.1f ms, decode %.1f ms\n", enc, dec);
    }
    if (qEnvironmentVariableIsSet("DT_AMBE_PERM")) {
        // Same speech through rate 33 (72 bits with FEC) and rate 34 (49 bits), each from a fresh
        // encoder; unpack the 72-bit frames with mbelib's FEC and match the bits up.
        std::vector<std::array<char, 49>> mb;
        std::vector<std::array<uint8_t, 49>> dv;
        std::vector<float> out;
        mbe_parms cur{}, prev{}, enh{};
        mbe_initMbeParms(&cur, &prev, &enh);
        chip->rawRate(QByteArray::fromHex("0921"));
        for (size_t f = 0; f < frames; ++f) {
            QByteArray b;
            int n = 0;
            if (!chip->encodeRaw(in.data() + f * 160, b, n) || n != 72) { fprintf(stdout, "rate 33 encode failed\n"); break; }
            char fr[4][24];
            std::memset(fr, 0, sizeof fr);
            auto bit = [&](int i) { return char((uint8_t(b[i >> 3]) >> (7 - (i & 7))) & 1); };
            char onAir[72];
            for (int k = 0; k < 72; ++k) onAir[k] = bit(k);
            dmrDeinterleave(onAir, fr);   // hypothesis: rate 33 bits are in DMR on-air order
            std::array<char, 49> d{};
            float fl[160];
            int e1 = 0, e2 = 0;
            char es[64];
            mbe_processAmbe3600x2450Framef(fl, &e1, &e2, es, fr, d.data(), &cur, &prev, &enh, 3);
            out.insert(out.end(), fl, fl + 160);
            mb.push_back(d);
        }
        fprintf(stdout, "  rate 33 (72 bits, DMR on-air order) -> mbelib     match %.2f\n", match(ref, out));
        chip->rawRate(QByteArray::fromHex("0922"));
        for (size_t f = 0; f < mb.size(); ++f) {
            std::array<uint8_t, 49> b{};
            if (!chip->encode2450(in.data() + f * 160, b.data())) break;
            dv.push_back(b);
        }
        // For each chip bit, the mbelib bit that equals it in every frame
        QStringList perm;
        int found = 0;
        for (int j = 0; j < 49; ++j) {
            QList<int> cand;
            for (int i = 0; i < 49; ++i) {
                bool same = true, varies = false;
                for (size_t f = 0; f < std::min(mb.size(), dv.size()) && same; ++f) {
                    same = (mb[f][size_t(i)] != 0) == (dv[f][size_t(j)] != 0);
                    varies = varies || dv[f][size_t(j)] != dv[0][size_t(j)];
                }
                if (same && varies) cand << i;
            }
            perm << (cand.size() == 1 ? QString::number(cand[0]) : cand.isEmpty() ? QString("?") : QString("{%1}").arg(cand.size()));
            found += cand.size() == 1;
        }
        fprintf(stdout, "  chip bit j = mbelib bit: %s\n  (%d of 49 unique)\n", qPrintable(perm.join(',')), found);
    }
    if (qEnvironmentVariableIsSet("DT_AMBE_RATES")) {   // explore: chip round trip per rate
        for (const QString &r : qEnvironmentVariable("DT_AMBE_RATES").split(',')) {
            if (!chip->rawRate(QByteArray::fromHex(r.toLatin1()))) { fprintf(stdout, "  rate %s: refused\n", qPrintable(r)); continue; }
            std::vector<float> o;
            int nbits = 0;
            bool ok = true;
            for (size_t f = 0; f < frames && ok; ++f) {
                int16_t pcm[160];
                ok = chip->roundTrip(in.data() + f * 160, pcm, nbits);
                o.insert(o.end(), pcm, pcm + 160);
            }
            fprintf(stdout, "  rate %-28s %2d bits  %s match %.2f\n", qPrintable(r), nbits, ok ? "ok  " : "FAIL", ok ? match(ref, o) : 0.0);
            fflush(stdout);
        }
    }
    fprintf(stdout, allOk ? "AMBE self-test passed\n" : "AMBE self-test FAILED\n");
    return allOk ? 0 : 2;
}
