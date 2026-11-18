#pragma once
// The radio's front panel: what the display and LEDs show, and what the controls do.
//
// It knows nothing about the hardware. Displays implement Display, LED sets implement
// Indicators, and input devices call press(), release(), turn() and togglePtt(); main.cpp does
// the wiring. The hardware (docs/wiring.md):
//
//   GpioPanel   mode buttons, Menu, PTT and the two rotary encoders (with their push switches)
//   LedPanel    the six mode LEDs and the status LED
//   Lcd2004     the 20x4 display (four-line layout)
//   LcdPlate    the Adafruit 16x2 plate and its five buttons, if the 20x4 isn't fitted (two-line layout)
//   headset     call button: togglePtt()
//
// Controls:
//   Mode button        tap: listen to the mode or not; hold: make it the TX mode
//   Channel knob       turn: step through the TX mode's destinations (reflectors, talkgroups,
//                      DMR talkgroups from settings.json), starting at the current one;
//                      push: go there; push and turn (D-Star, M17): module letter
//   Volume knob        turn: volume; push: mute
//   PTT                hold to talk (headset button: press on, press off)
//   Menu               tap: back (stop browsing); hold 3 s: shut down (shutdownRequested())
//   Plate Select       PTT, or go to the destination shown while browsing
//   Plate Up / Down    TX mode; while browsing D-Star or M17, the module letter
//   Plate Left / Right step through destinations (hold to scroll)
//
// LEDs:
//   mode    green = listening, red = TX mode, yellow = both, blue = call on air, off = not listening
//   status  red = transmitting, green = receiving, blue = network problem or unread page
#include <QElapsedTimer>
#include <QList>
#include <QObject>
#include <QPair>
#include <QString>
#include <QStringList>

class Radio;

class Display {
public:
    // Colours, also used for the LEDs (red = 4, green = 2, blue = 1)
    enum Colour { Off = 0, Blue = 1, Green = 2, Cyan = 3, Red = 4, Violet = 5, Yellow = 6, White = 7 };
    virtual ~Display() = default;
    virtual int columns() const = 0;
    virtual int rows() const = 0;
    virtual void show(const QStringList &lines) = 0;   // rows() lines; only writes what changed
    virtual void setColour(int rgb) = 0;               // backlight (on / off for a single colour)
    void keepOnExit() { keep_ = true; }                // leave the text and backlight on when closed

    // A line as an HD44780 shows it: printable ASCII, padded or cut to the width
    static QString fit(const QString &s, int width)
    {
        QString out;
        for (QChar c : s) {
            if (out.size() == width) break;
            const ushort u = c.unicode();
            out += u >= 32 && u < 127 ? c : u == 0x2192 ? QChar('>') : QChar('?');   // "→" -> ">"
        }
        return out.leftJustified(width, ' ');
    }

protected:
    bool keep_ = false;
};

class Indicators {
public:
    enum Led { DMR, DStar, YSF, P25, NXDN, M17, Status, Count };   // modes in Radio::kModes order
    virtual ~Indicators() = default;
    virtual void setLed(int led, int rgb) = 0;   // Display::Colour
    virtual void commit() {}                     // after a round of setLed()
};

class FrontPanel : public QObject {
    Q_OBJECT
public:
    enum class Key {
        Select, Up, Down, Left, Right,             // LCD plate
        Ptt, Mute, Link, Menu,                     // PTT, volume push, channel push, Menu
        DMR, DStar, YSF, P25, NXDN, M17,           // mode buttons, in Radio::kModes order
    };
    enum class Knob { Tune, Volume };

    FrontPanel(Radio &radio, Display &display, QObject *parent = nullptr);
    void setIndicators(Indicators *leds) { leds_ = leds; }
    void setModeColours(bool on) { modeColours_ = on; }   // display backlight in the mode's colour

    // Controls
    void press(Key k);
    void release(Key k);
    void turn(Knob k, int steps);   // + clockwise
    void togglePtt();

    void message(const QString &line1, const QString &line2, int ms);   // shown for a while

signals:
    void shutdownRequested();       // Menu held: the radio should shut down and power off

private:
    void refresh();                 // after every radio tick
    QStringList screen2(qint64 now) const;   // 16x2 layout
    QStringList screen4(qint64 now);         // 20x4 layout
    QString statusLine();                    // 20x4 bottom line: IP address, volume, or a problem
    void updateLeds();
    bool browsing() const { return !browse_.isEmpty(); }
    void browse(int dir);
    void buildBrowseList(const QString &mode);
    void choose();                  // go to the destination shown
    void stepModule(int dir);
    void stepTxMode(int dir);
    void modeTap(const QString &mode);
    void modeHold(const QString &mode);
    QString destination(const QString &mode) const;   // where PTT in this mode goes
    int modeColour(const QString &mode) const;
    QString twoParts(const QString &left, const QString &right) const;

    Radio &radio_;
    Display &display_;
    Indicators *leds_ = nullptr;
    bool modeColours_ = false;

    // Browsing the TX mode's destinations
    QList<QPair<QString, QString>> browse_;   // (target, label)
    QString browseMode_;
    int browseIdx_ = 0;
    qint64 browseUntil_ = 0;
    QElapsedTimer lastTurn_;        // channel knob speed, for faster scrolling

    QString msg_[2];
    qint64 msgUntil_ = 0;
    QString lastHeard_, lastHeardName_, lastHeardTime_;
    QString ip_;
    qint64 ipCheckedMs_ = 0;
    bool pageUnread_ = false;

    int held_ = -1;                 // plate Left / Right held (repeat)
    qint64 heldSince_ = 0, repeatAt_ = 0;
    bool selectChose_ = false;      // this Select press chose a destination, so its release isn't PTT off
    int modeHeld_ = -1;             // mode button held (index into Radio::kModes)
    qint64 modeHeldSince_ = 0;
    bool modeHoldDone_ = false;
    bool linkHeld_ = false, linkTurned_ = false;   // channel knob pushed (and turned while pushed)
    bool menuHeld_ = false;
    qint64 menuHeldSince_ = 0;
    bool shuttingDown_ = false;     // controls are ignored from now on
};
