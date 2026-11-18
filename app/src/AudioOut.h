#pragma once
// Built-in audio output: 16-bit mono 8 kHz PCM straight to the sound server.
// PulseAudio (also served by PipeWire) through libpulse-simple, falling back to
// ALSA through libasound. Both are loaded at run time, so nothing extra is needed
// to build, and no external player program is used.
// Writes never block the caller: samples are queued and a worker thread feeds
// the device.
#include <QString>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

class AudioOut {
public:
    explicit AudioOut(const QString &streamName);
    ~AudioOut();
    AudioOut(const AudioOut &) = delete;
    AudioOut &operator=(const AudioOut &) = delete;

    // backend: "auto", "pulse" or "alsa"; device: Pulse sink / ALSA device name, empty = default.
    bool open(const QString &backend, const QString &device, QString *err = nullptr);
    void close();
    bool isOpen() const { return worker_.joinable(); }
    QString backendName() const { return backendName_; }

    void write(const int16_t *samples, size_t n);   // queue for playback
    void flush();                                   // drop everything not yet played
    double queuedSeconds() const;                   // still waiting in our queue

private:
    void run();
    bool openPulse(const QString &device, QString *err);
    bool openAlsa(const QString &device, QString *err);
    bool deviceWrite(const int16_t *s, size_t n);
    void deviceFlush();
    void deviceClose();

    QString name_, backendName_;
    void *pulse_ = nullptr;   // pa_simple *
    void *alsa_ = nullptr;    // snd_pcm_t *

    mutable std::mutex m_;
    std::condition_variable cv_;
    std::deque<std::vector<int16_t>> queue_;
    size_t queuedSamples_ = 0;
    bool stop_ = false, flush_ = false;
    std::thread worker_;
};
