#pragma once
// Built-in microphone capture: 8 kHz mono 16-bit from PulseAudio / PipeWire
// (libpulse-simple, loaded at run time), delivered in 20 ms frames on the
// receiver's thread.
#include <QObject>
#include <QString>
#include <atomic>
#include <functional>
#include <thread>
#include <vector>

class AudioIn {
public:
    using FrameFn = std::function<void(const std::vector<int16_t> &frame160)>;

    ~AudioIn();
    // device: PulseAudio source name, empty = default. onFrame runs on receiver's thread.
    bool start(const QString &device, QObject *receiver, FrameFn onFrame, QString *err = nullptr);
    void stop();
    bool running() const { return worker_.joinable(); }

private:
    void *pa_ = nullptr;   // pa_simple *
    std::atomic<bool> stop_{false};
    std::thread worker_;
};
