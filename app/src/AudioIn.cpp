#include "AudioIn.h"
#include <atomic>
#include <memory>

#include <QMetaObject>
#include <QPointer>
#include <dlfcn.h>

namespace {
struct PaSampleSpec { int format; uint32_t rate; uint8_t channels; };
struct PaBufferAttr { uint32_t maxlength, tlength, prebuf, minreq, fragsize; };
constexpr int PA_STREAM_RECORD = 2;
constexpr int PA_SAMPLE_S16LE = 3;

struct Pulse {
    void *(*newStream)(const char *, const char *, int, const char *, const char *, const PaSampleSpec *,
                       const void *, const PaBufferAttr *, int *) = nullptr;
    int (*read)(void *s, void *data, size_t bytes, int *error) = nullptr;
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
    p.read = reinterpret_cast<decltype(p.read)>(dlsym(h, "pa_simple_read"));
    p.free = reinterpret_cast<decltype(p.free)>(dlsym(h, "pa_simple_free"));
    if (core) p.strerror = reinterpret_cast<decltype(p.strerror)>(dlsym(core, "pa_strerror"));
    p.ok = p.newStream && p.read && p.free;
    return p;
}
}   // namespace

AudioIn::~AudioIn() { stop(); }

bool AudioIn::start(const QString &device, QObject *receiver, FrameFn onFrame, QString *err)
{
    stop();
    const Pulse &p = pulse();
    if (!p.ok) {
        if (err) *err = "PulseAudio (libpulse-simple.so.0) not found";
        return false;
    }
    PaSampleSpec ss{PA_SAMPLE_S16LE, 8000, 1};
    PaBufferAttr attr{uint32_t(-1), uint32_t(-1), uint32_t(-1), uint32_t(-1), 320};   // 20 ms fragments
    int e = 0;
    const QByteArray dev = device.toUtf8();
    pa_ = p.newStream(nullptr, "Digital Transceiver", PA_STREAM_RECORD, dev.isEmpty() ? nullptr : dev.constData(),
                      "Microphone", &ss, nullptr, &attr, &e);
    if (!pa_) {
        if (err) *err = QString("microphone: %1").arg(p.strerror ? p.strerror(e) : QString::number(e));
        return false;
    }
    stop_ = false;
    QPointer<QObject> rx(receiver);
    // At most 5 frames (100 ms) wait for the main thread: if it falls behind (e.g. a slow
    // vocoder), newer frames are dropped rather than queued, so its timers and buttons
    // (PTT release!) still get handled promptly.
    auto pending = std::make_shared<std::atomic<int>>(0);
    worker_ = std::thread([this, rx, onFrame, pending] {
        std::vector<int16_t> buf(160);
        while (!stop_) {
            int e2 = 0;
            if (pulse().read(pa_, buf.data(), buf.size() * 2, &e2) < 0) break;
            if (stop_ || !rx) break;
            if (*pending >= 5) continue;
            ++*pending;
            std::vector<int16_t> frame = buf;
            QMetaObject::invokeMethod(rx.data(), [onFrame, frame, pending] { --*pending; onFrame(frame); }, Qt::QueuedConnection);
        }
    });
    return true;
}

void AudioIn::stop()
{
    stop_ = true;
    if (worker_.joinable()) worker_.join();   // pa_simple_read returns within one 20 ms fragment
    if (pa_) {
        pulse().free(pa_);
        pa_ = nullptr;
    }
}
