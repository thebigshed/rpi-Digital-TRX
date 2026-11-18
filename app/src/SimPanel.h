#pragma once
// A stand-in for the front-panel hardware, for running on a PC (--sim: the 20x4 display;
// --sim-16x2: the LCD plate's): the display and the LEDs are printed to stdout when they change,
// and controls are read from stdin, one per line:
//
//   s+ s-  u+ u-  d+ d-  l+ l-  r+ r-   plate Select, Up, Down, Left, Right pressed (+) / released (-)
//   1+ 1- ... 6+ 6-                     mode buttons DMR, D-Star, YSF, P25, NXDN, M17
//   p+ p-                               PTT button
//   k+ k-                               channel knob push
//   n+ n-                               Menu
//   t+ t-  v+ v-                        channel / volume knob one step clockwise (+) / anticlockwise (-)
//   m                                   volume knob push (mute)
//   h                                   headset call button (toggles PTT)
#include <QObject>
#include <QString>

#include "FrontPanel.h"

class QSocketNotifier;

class SimPanel : public QObject, public Display, public Indicators {
    Q_OBJECT
public:
    SimPanel(int columns, int rows, QObject *parent = nullptr);
    void attach(FrontPanel *panel) { panel_ = panel; }

    int columns() const override { return cols_; }
    int rows() const override { return rows_; }
    void show(const QStringList &lines) override;
    void setColour(int rgb) override;
    void setLed(int led, int rgb) override { leds_[led] = rgb; }
    void commit() override;

private:
    void line(const QString &l);

    FrontPanel *panel_ = nullptr;
    QSocketNotifier *stdin_ = nullptr;
    QByteArray input_;
    int cols_, rows_;
    QStringList shown_;
    int colour_ = -1;
    int leds_[Count] = {}, shownLeds_[Count] = {-1};
};
