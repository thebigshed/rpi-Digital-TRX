#include "AudioOut.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <dlfcn.h>

// ---------------------------------------------------------------- PulseAudio simple API (stable C ABI)
namespace {
struct PaSampleSpec { int format; uint32_t rate; uint8_t channels; };
struct PaBufferAttr { uint32_t maxlength, tlength, prebuf, minreq, fragsize; };
constexpr int PA_STREAM_PLAYBACK = 1;
constexpr int PA_SAMPLE_S16LE = 3;

struct Pulse {
    void *(*newStream)(const char *server, const char *name, int dir, const char *dev, const char *streamName,
                       const PaSampleSpec *ss, const void *map, const PaBufferAttr *attr, int *error) = nullptr;
    int (*write)(void *s, const void *data, size_t bytes, int *error) = nullptr;
    int (*flush)(void *s, int *error) = nullptr;
    void (*free)(void *s) = nullptr;
    const char *(*strerror)(int error) = nullptr;
    bool ok = false;
};

const Pulse &pulse()
{
    static Pulse p;
    static bool tried = false;
    if (tried) return p;
    tried = true;
    void *h = dlopen("libpulse-simple.so.0", RTLD_NOW);
    void *core = dlopen("libpulse.so.0", RTLD_NOW);
    if (!h) return p;
    p.newStream = reinterpret_cast<decltype(p.newStream)>(dlsym(h, "pa_simple_new"));
    p.write = reinterpret_cast<decltype(p.write)>(dlsym(h, "pa_simple_write"));
    p.flush = reinterpret_cast<decltype(p.flush)>(dlsym(h, "pa_simple_flush"));
    p.free = reinterpret_cast<decltype(p.free)>(dlsym(h, "pa_simple_free"));
    if (core) p.strerror = reinterpret_cast<decltype(p.strerror)>(dlsym(core, "pa_strerror"));
    p.ok = p.newStream && p.write && p.free;
    return p;
}

// ---------------------------------------------------------------- ALSA (stable C ABI)
constexpr int SND_PCM_STREAM_PLAYBACK = 0;
constexpr int SND_PCM_FORMAT_S16_LE = 2;
constexpr int SND_PCM_ACCESS_RW_INTERLEAVED = 3;

struct Alsa {
    int (*open)(void **pcm, const char *name, int stream, int mode) = nullptr;
    int (*setParams)(void *pcm, int format, int access, unsigned channels, unsigned rate, int softResample,
                     unsigned latencyUs) = nullptr;
    long (*writei)(void *pcm, const void *buf, unsigned long frames) = nullptr;
    int (*recover)(void *pcm, int err, int silent) = nullptr;
    int (*drop)(void *pcm) = nullptr;
    int (*prepare)(void *pcm) = nullptr;
    int (*close)(void *pcm) = nullptr;
    const char *(*strerror)(int err) = nullptr;
    bool ok = false;
};

const Alsa &alsa()
{
    static Alsa a;
    static bool tried = false;
    if (tried) return a;
    tried = true;
    void *h = dlopen("libasound.so.2", RTLD_NOW);
    if (!h) return a;
    a.open = reinterpret_cast<decltype(a.open)>(dlsym(h, "snd_pcm_open"));
    a.setParams = reinterpret_cast<decltype(a.setParams)>(dlsym(h, "snd_pcm_set_params"));
    a.writei = reinterpret_cast<decltype(a.writei)>(dlsym(h, "snd_pcm_writei"));
    a.recover = reinterpret_cast<decltype(a.recover)>(dlsym(h, "snd_pcm_recover"));
    a.drop = reinterpret_cast<decltype(a.drop)>(dlsym(h, "snd_pcm_drop"));
    a.prepare = reinterpret_cast<decltype(a.prepare)>(dlsym(h, "snd_pcm_prepare"));
    a.close = reinterpret_cast<decltype(a.close)>(dlsym(h, "snd_pcm_close"));
    a.strerror = reinterpret_cast<decltype(a.strerror)>(dlsym(h, "snd_strerror"));
    a.ok = a.open && a.setParams && a.writei && a.recover && a.drop && a.prepare && a.close;
    return a;
}

constexpr unsigned kRate = 8000;
constexpr unsigned kLatencyMs = 200;   // device buffer: absorbs network jitter between voice packets
}   // namespace

AudioOut::AudioOut(const QString &streamName) : name_(streamName) {}

AudioOut::~AudioOut() { close(); }

bool AudioOut::open(const QString &backend, const QString &device, QString *err)
{
    close();
    QString e1, e2;
    const bool tryPulse = backend != "alsa", tryAlsa = backend != "pulse";
    bool ok = (tryPulse && openPulse(device, &e1)) || (tryAlsa && openAlsa(device, &e2));
    if (!ok) {
        if (err) *err = e1.isEmpty() ? e2 : e2.isEmpty() ? e1 : e1 + "; " + e2;
        return false;
    }
    stop_ = false;
    worker_ = std::thread([this] { run(); });
    return true;
}

bool AudioOut::openPulse(const QString &device, QString *err)
{
    const Pulse &p = pulse();
    if (!p.ok) { *err = "PulseAudio: libpulse-simple.so.0 not found"; return false; }
    PaSampleSpec ss{PA_SAMPLE_S16LE, kRate, 1};
    const uint32_t bytes = kRate * 2 * kLatencyMs / 1000;
    PaBufferAttr attr{uint32_t(-1), bytes, uint32_t(-1), uint32_t(-1), uint32_t(-1)};
    int e = 0;
    const QByteArray dev = device.toUtf8(), name = name_.toUtf8();
    pulse_ = p.newStream(nullptr, "Digital Transceiver", PA_STREAM_PLAYBACK, dev.isEmpty() ? nullptr : dev.constData(),
                         name.constData(), &ss, nullptr, &attr, &e);
    if (!pulse_) {
        *err = QString("PulseAudio: %1").arg(p.strerror ? p.strerror(e) : QString::number(e));
        return false;
    }
    backendName_ = "PulseAudio";
    return true;
}

bool AudioOut::openAlsa(const QString &device, QString *err)
{
    const Alsa &a = alsa();
    if (!a.ok) { *err = "ALSA: libasound.so.2 not found"; return false; }
    const QByteArray dev = device.isEmpty() ? QByteArray("default") : device.toUtf8();
    int r = a.open(&alsa_, dev.constData(), SND_PCM_STREAM_PLAYBACK, 0);
    if (r >= 0) r = a.setParams(alsa_, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED, 1, kRate, 1, kLatencyMs * 1000);
    if (r < 0) {
        *err = QString("ALSA %1: %2").arg(QString::fromUtf8(dev), a.strerror ? a.strerror(r) : QString::number(r));
        if (alsa_) { a.close(alsa_); alsa_ = nullptr; }
        return false;
    }
    backendName_ = "ALSA";
    return true;
}

void AudioOut::close()
{
    if (worker_.joinable()) {
        {
            std::lock_guard<std::mutex> l(m_);
            stop_ = true;
            flush_ = true;
        }
        cv_.notify_all();
        worker_.join();
    }
    deviceClose();
    std::lock_guard<std::mutex> l(m_);
    queue_.clear();
    queuedSamples_ = 0;
    stop_ = flush_ = false;
}

void AudioOut::write(const int16_t *samples, size_t n)
{
    if (!n || !worker_.joinable()) return;
    {
        std::lock_guard<std::mutex> l(m_);
        // Don't let a stalled device build up minutes of backlog (recordings are allowed in full).
        if (queuedSamples_ > kRate * 60 * 15) return;
        // Queue in 40 ms pieces: a device write blocks until its piece is played, so small
        // pieces let flush()/close() take effect almost at once instead of after the whole clip.
        constexpr size_t kPiece = kRate / 25;
        for (size_t off = 0; off < n; off += kPiece) {
            const size_t len = std::min(kPiece, n - off);
            queue_.emplace_back(samples + off, samples + off + len);
        }
        queuedSamples_ += n;
    }
    cv_.notify_one();
}

void AudioOut::flush()
{
    {
        std::lock_guard<std::mutex> l(m_);
        queue_.clear();
        queuedSamples_ = 0;
        flush_ = true;
    }
    cv_.notify_one();
}

double AudioOut::queuedSeconds() const
{
    std::lock_guard<std::mutex> l(m_);
    return double(queuedSamples_) / kRate;
}

void AudioOut::run()
{
    for (;;) {
        std::vector<int16_t> chunk;
        bool doFlush = false;
        {
            std::unique_lock<std::mutex> l(m_);
            cv_.wait(l, [this] { return stop_ || flush_ || !queue_.empty(); });
            if (flush_) { doFlush = true; flush_ = false; }
            if (stop_) return;
            if (!doFlush && !queue_.empty()) {
                chunk = std::move(queue_.front());
                queue_.pop_front();
                queuedSamples_ -= chunk.size();
            }
        }
        if (doFlush) { deviceFlush(); continue; }
        if (!chunk.empty()) deviceWrite(chunk.data(), chunk.size());   // blocks only this thread
    }
}

bool AudioOut::deviceWrite(const int16_t *s, size_t n)
{
    if (pulse_) {
        int e = 0;
        return pulse().write(pulse_, s, n * 2, &e) >= 0;
    }
    if (alsa_) {
        const Alsa &a = alsa();
        while (n > 0) {
            long r = a.writei(alsa_, s, n);
            if (r < 0) {
                if (a.recover(alsa_, int(r), 1) < 0) return false;   // underrun between calls: recover quietly
                continue;
            }
            s += r;
            n -= size_t(r);
        }
        return true;
    }
    return false;
}

void AudioOut::deviceFlush()
{
    if (pulse_ && pulse().flush) {
        int e = 0;
        pulse().flush(pulse_, &e);
    }
    if (alsa_) {
        alsa().drop(alsa_);
        alsa().prepare(alsa_);
    }
}

void AudioOut::deviceClose()
{
    if (pulse_) { pulse().free(pulse_); pulse_ = nullptr; }
    if (alsa_) { alsa().drop(alsa_); alsa().close(alsa_); alsa_ = nullptr; }
    backendName_.clear();
}
