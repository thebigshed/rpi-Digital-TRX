#include "GpioPanel.h"
#include "Gpio.h"
#include "gpio_v2.h"

#include <QSocketNotifier>
#include <unistd.h>

namespace {
using Key = FrontPanel::Key;

struct Button { unsigned gpio; Key key; };
constexpr Button kButtons[] = {
    {5, Key::DMR}, {6, Key::DStar}, {12, Key::YSF}, {13, Key::P25}, {19, Key::NXDN}, {16, Key::M17},
    {26, Key::Menu}, {23, Key::Ptt}, {22, Key::Link}, {18, Key::Mute}};
constexpr unsigned kDebounceUs = 10000;
}

GpioPanel::GpioPanel(QObject *parent) : QObject(parent)
{
    // Channel CLK is on GPIO 24, not 17: the DV3000 holds GPIO 17 (header pin 11) low.
    encoders_ = {{FrontPanel::Knob::Tune, 24, 27}, {FrontPanel::Knob::Volume, 20, 21}};
}

GpioPanel::~GpioPanel()
{
    if (buttonFd_ >= 0) ::close(buttonFd_);
    if (encoderFd_ >= 0) ::close(encoderFd_);
}

bool GpioPanel::open(QString *err)
{
    QList<unsigned> buttons, encoderLines;
    for (const Button &b : kButtons) buttons << b.gpio;
    for (const Encoder &e : encoders_) encoderLines << e.clk << e.dt;

    buttonFd_ = Gpio::requestInputs(buttons, kDebounceUs, err);
    // The encoders aren't debounced by the kernel (it would delay and drop their quick edges);
    // the decoder below ignores contact bounce instead.
    encoderFd_ = Gpio::requestInputs(encoderLines, 0, err);
    if (buttonFd_ < 0 || encoderFd_ < 0) return false;

    const uint64_t v = Gpio::values(encoderFd_, int(encoderLines.size()));
    for (int i = 0; i < encoderLines.size(); ++i)
        if (v & (1ULL << i)) levels_ |= 1ULL << encoderLines[i];
    for (Encoder &e : encoders_)
        e.state = int(((levels_ >> e.clk) & 1) << 1 | ((levels_ >> e.dt) & 1));

    buttonN_ = new QSocketNotifier(buttonFd_, QSocketNotifier::Read, this);
    connect(buttonN_, &QSocketNotifier::activated, this, &GpioPanel::buttonEvents);
    encoderN_ = new QSocketNotifier(encoderFd_, QSocketNotifier::Read, this);
    connect(encoderN_, &QSocketNotifier::activated, this, &GpioPanel::encoderEvents);
    return true;
}

void GpioPanel::buttonEvents()
{
    gpio_v2_line_event ev[16];
    const ssize_t n = ::read(buttonFd_, ev, sizeof ev);
    for (ssize_t i = 0; i < n / ssize_t(sizeof ev[0]); ++i)
        for (const Button &b : kButtons)
            if (b.gpio == ev[i].offset) {
                // Buttons pull the pin to GND: falling = pressed.
                if (ev[i].id == GPIO_V2_LINE_EVENT_FALLING_EDGE) emit pressed(b.key);
                else emit released(b.key);
            }
}

void GpioPanel::encoderEvents()
{
    gpio_v2_line_event ev[32];
    const ssize_t n = ::read(encoderFd_, ev, sizeof ev);
    for (ssize_t i = 0; i < n / ssize_t(sizeof ev[0]); ++i) {
        const quint64 bit = 1ULL << ev[i].offset;
        if (ev[i].id == GPIO_V2_LINE_EVENT_RISING_EDGE) levels_ |= bit; else levels_ &= ~bit;
        for (Encoder &e : encoders_)
            if (ev[i].offset == e.clk || ev[i].offset == e.dt)
                step(e, int(((levels_ >> e.clk) & 1) << 1 | ((levels_ >> e.dt) & 1)));
    }
}

// Quadrature decoding. Each valid change of CLK/DT is a quarter step one way or the other;
// bounce adds and then takes away again. A KY-040 rests with both lines high between detents,
// so a step is counted when it gets back there having moved at least half way.
void GpioPanel::step(Encoder &e, int ab)
{
    static const int8_t quarter[16] = {0, -1, 1, 0, 1, 0, 0, -1, -1, 0, 0, 1, 0, 1, -1, 0};
    e.acc += quarter[(e.state << 2) | ab];
    e.state = ab;
    if (ab != 3) return;
    if (e.acc >= 2) emit turned(e.knob, 1);
    else if (e.acc <= -2) emit turned(e.knob, -1);
    e.acc = 0;
}
