#pragma once
// The front panel's buttons and rotary encoders, wired straight to the Pi's GPIO pins (see
// docs/wiring.md). The kernel reports each edge as an event, debounces the buttons and pulls
// every input up, so there is no polling.
//
//   GPIO  5  6 12 13 19 16   mode buttons: DMR, D-Star, YSF, P25, NXDN, M17
//   GPIO 26                  Menu / Back
//   GPIO 23                  PTT (hold to talk)
//   GPIO 24 / 27, 22         channel encoder CLK / DT, push (not GPIO 17: the DV3000 holds it low)
//   GPIO 20 / 21, 18         volume encoder CLK / DT, push
// Buttons connect their pin to GND.
#include <QList>
#include <QObject>
#include <QString>

#include "FrontPanel.h"

class QSocketNotifier;

class GpioPanel : public QObject {
    Q_OBJECT
public:
    explicit GpioPanel(QObject *parent = nullptr);
    ~GpioPanel() override;
    bool open(QString *err);   // false if the GPIO lines can't be had (e.g. not a Pi)

signals:
    void pressed(FrontPanel::Key key);
    void released(FrontPanel::Key key);
    void turned(FrontPanel::Knob knob, int steps);   // + = clockwise

private:
    struct Encoder {
        FrontPanel::Knob knob;
        unsigned clk, dt;    // GPIO numbers
        int state = 3;       // CLK << 1 | DT; 3 = both high, the rest position
        int acc = 0;         // quarter steps since the last detent
    };
    void buttonEvents();
    void encoderEvents();
    void step(Encoder &e, int ab);

    int buttonFd_ = -1, encoderFd_ = -1;
    QSocketNotifier *buttonN_ = nullptr, *encoderN_ = nullptr;
    QList<Encoder> encoders_;
    quint64 levels_ = 0;     // encoder line levels, bit = GPIO number
};
