#pragma once
#include <QObject>
#include <QMap>
#include <QTimer>

class QSocketNotifier;

// The call button of a USB headset (e.g. the "Teams" button of a Poly Blackwire), read from the
// Linux input devices. Teams-certified headsets report it as BTN_0 from a vendor usage, and only
// as a click (pressed and released at once), so it toggles PTT rather than being held.
//
// Reading /dev/input needs root or membership of the "input" group. Devices are rescanned every
// 30 s, so a headset plugged in later is picked up.
class HeadsetButtons : public QObject {
    Q_OBJECT
public:
    explicit HeadsetButtons(QObject *parent = nullptr);
    ~HeadsetButtons() override;
    QStringList devices() const;   // names of the devices being watched

signals:
    void clicked();                 // the call button was pressed
    void devicesChanged();

private:
    void rescan();
    struct Dev { int fd = -1; QString name; QSocketNotifier *n = nullptr; };
    QMap<QString, Dev> devs_;       // by /dev/input/eventN
    QTimer rescan_;
    qint64 lastClickMs_ = 0;
};
