// Digital TRX: the radio, on a Raspberry Pi with the LCD plate and the AMBE board.
//
//   digital-transceiver [--ambe /dev/ttyAMA0] [--lcd-colour]
//   digital-transceiver --sim                     on a PC: 20x4 display and LEDs on stdout, controls on stdin
//   digital-transceiver --sim-16x2                the same, with the LCD plate's 16x2 layout
//   digital-transceiver --replay capture.bin      feed a capture (settings and logs untouched)
//   digital-transceiver --mic-test                microphone levels for 3 s
//   digital-transceiver --ambe DEV --ambe-test recording.wav
//   digital-transceiver --lcd-message LINE...      write to the display and leave it (used at the end
//                                                  of shutdown: "Safe to unplug")
//
// Wiring: Radio (the radio itself) <- FrontPanel (display, LEDs and controls) <- the hardware:
// the LCD plate, the panel's buttons and encoders (GpioPanel), its LEDs (LedPanel) and the headset.
#include "AmbeChip.h"
#include "FrontPanel.h"
#include "GpioPanel.h"
#include "LedPanel.h"
#include "HeadsetButtons.h"
#include "Lcd2004.h"
#include "LcdPlate.h"
#include "Radio.h"
#include "SimPanel.h"

#include <QCoreApplication>
#include <QProcess>
#include <QSocketNotifier>
#include <QTimer>
#include <cmath>
#include <csignal>
#include <memory>
#include <sys/socket.h>
#include <unistd.h>

int ambeSelfTest(const QString &spec, const QString &wav);   // AmbeSelfTest.cpp

namespace {
QString argValue(const QStringList &args, const QString &name)
{
    const int i = int(args.indexOf(name));
    return i > 0 && i + 1 < args.size() ? args[i + 1] : QString();
}

// Quit cleanly on SIGTERM / SIGINT (the init script stops the radio with SIGTERM), so the
// networks are unlinked and the settings saved. The handler only writes to a socket; the event
// loop does the rest.
int signalFds[2] = {-1, -1};

void quitOnSignals()
{
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, signalFds) != 0) return;
    auto *n = new QSocketNotifier(signalFds[1], QSocketNotifier::Read, qApp);
    QObject::connect(n, &QSocketNotifier::activated, qApp, [] {
        char c;
        if (::read(signalFds[1], &c, 1) > 0) QCoreApplication::quit();
    });
    struct sigaction sa = {};
    sa.sa_handler = [](int) { const char c = 1; (void)!::write(signalFds[0], &c, 1); };
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sigaction(SIGTERM, &sa, nullptr);
    sigaction(SIGINT, &sa, nullptr);
}

// Show some lines on whichever display is fitted, and leave them there. Runs at the very end of
// shutdown, when the filesystems are read-only, so it uses nothing but the display.
int lcdMessage(const QStringList &lines)
{
    std::unique_ptr<Display> d;
    auto lcd = std::make_unique<Lcd2004>();
    if (lcd->open()) {
        d = std::move(lcd);
    } else {
        auto plate = std::make_unique<LcdPlate>();
        if (!plate->open(1, 0x20)) return 1;
        d = std::move(plate);
    }
    d->setColour(Display::White);
    d->show(lines);
    d->keepOnExit();
    return 0;
}

int micTest()
{
    const Settings cfg = Settings::load();
    auto mic = std::make_shared<AudioIn>();
    auto levels = std::make_shared<QStringList>();
    QString err;
    if (!mic->start(cfg.transmit.inputDevice, qApp, [levels](const std::vector<int16_t> &f) {
            double acc = 0, pk = 0;
            for (int16_t v : f) { acc += double(v) * v; pk = std::max(pk, std::abs(double(v))); }
            if (levels->size() < 1000) *levels << QString("%1/%2").arg(int(std::sqrt(acc / f.size()))).arg(int(pk));
        }, &err)) {
        fprintf(stdout, "MIC ERROR %s\n", qPrintable(err));
        return 1;
    }
    QTimer::singleShot(3000, qApp, [mic, levels] {
        mic->stop();
        fprintf(stdout, "MIC frames=%d rms/peak: %s\n", int(levels->size()), qPrintable(levels->mid(0, 150).join(' ')));
        QCoreApplication::quit();
    });
    return QCoreApplication::exec();
}
}

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);   // writes nothing to disk, so --lcd-message is safe on a read-only root
    if (argc > 1 && QByteArray(argv[1]) == "--lcd-message") {
        QStringList lines;
        for (int i = 2; i < argc; ++i) lines << QString::fromLocal8Bit(argv[i]);
        return lcdMessage(lines);
    }
    // The same settings and log folders as the desktop app, so its settings.json can be copied over.
    QCoreApplication::setOrganizationName("digital-transceiver");
    QCoreApplication::setApplicationName("digital-transceiver");

    const QStringList args = QCoreApplication::arguments();
    const QString ambe = argValue(args, "--ambe");
    if (args.contains("--ambe-test"))
        return ambeSelfTest(ambe.isEmpty() ? QString("/dev/ttyAMA0") : ambe, argValue(args, "--ambe-test"));
    if (args.contains("--mic-test")) return micTest();

    const QString replay = argValue(args, "--replay");
    Radio radio(replay.isEmpty());

    // AMBE vocoder chip (otherwise the software vocoders)
    if (!ambe.isEmpty()) {
        QString err;
        if (AmbeChip *chip = AmbeChip::open(ambe, &err))
            radio.logEvent("INFO", QString("AMBE chip %1 (%2) on %3").arg(chip->productId(), chip->version(), ambe));
        else
            radio.logEvent("ERROR", "AMBE chip: " + err + "; using software vocoders");
    }

    // Display and controls
    std::unique_ptr<Display> display;
    LcdPlate *plate = nullptr;
    SimPanel *sim = nullptr;
    if (args.contains("--sim") || args.contains("--sim-16x2")) {
        display.reset(sim = args.contains("--sim-16x2") ? new SimPanel(16, 2) : new SimPanel(20, 4));
    } else {
        // The 20x4 display if it's fitted, otherwise the Adafruit plate.
        QString err, plateErr;
        auto lcd = std::make_unique<Lcd2004>();
        if (lcd->open(&err)) {
            radio.logEvent("INFO", QString("20x4 display at 0x%1").arg(lcd->address(), 2, 16));
            display = std::move(lcd);
        } else {
            plate = new LcdPlate;
            display.reset(plate);
            if (plate->open(1, 0x20, &plateErr)) radio.logEvent("INFO", "LCD plate started (" + err + ")");
            else radio.logEvent("ERROR", "No display: " + err + "; LCD plate: " + plateErr);   // runs without one
        }
    }
    FrontPanel panel(radio, *display);
    panel.setModeColours(args.contains("--lcd-colour"));
    std::unique_ptr<LedPanel> leds;
    if (sim) {
        sim->attach(&panel);
        panel.setIndicators(sim);
    }
    if (plate) {
        QObject::connect(plate, &LcdPlate::pressed, &panel, &FrontPanel::press);
        QObject::connect(plate, &LcdPlate::released, &panel, &FrontPanel::release);
    }
    if (!sim) {
        // The panel: buttons and encoders on GPIO, LEDs on the MCP23017 and GPIO. Each part is optional.
        QString err;
        auto *gpio = new GpioPanel(&app);
        if (gpio->open(&err)) {
            QObject::connect(gpio, &GpioPanel::pressed, &panel, &FrontPanel::press);
            QObject::connect(gpio, &GpioPanel::released, &panel, &FrontPanel::release);
            QObject::connect(gpio, &GpioPanel::turned, &panel, &FrontPanel::turn);
            radio.logEvent("INFO", "Panel buttons and encoders started");
        } else {
            radio.logEvent("ERROR", "Panel buttons: " + err);
        }
        leds = std::make_unique<LedPanel>();
        err.clear();
        if (leds->open(&err)) {
            panel.setIndicators(leds.get());
            radio.logEvent("INFO", "Panel LEDs: " + leds->describe());
        }
        if (!err.isEmpty()) radio.logEvent("ERROR", "Panel LEDs: " + err);
    }
    if (replay.isEmpty()) {   // the headset's call button ("Teams") toggles PTT
        auto *headset = new HeadsetButtons(&app);
        QObject::connect(headset, &HeadsetButtons::clicked, &panel, &FrontPanel::togglePtt);
        auto logDevices = [headset, &radio] {
            const QStringList d = headset->devices();
            radio.logEvent("INFO", d.isEmpty() ? QString("Headset call button: none found")
                                               : "Headset call button toggles PTT: " + d.join(", "));
        };
        QObject::connect(headset, &HeadsetButtons::devicesChanged, &radio, logDevices);
        logDevices();
    }
    if (replay.isEmpty()) radio.start();
    else radio.startReplay(replay);

    // Menu held: shut down. On the radio that powers the Pi off (S90dtrx stops this app on the
    // way, and the display is left showing "Shutting down" until the end of shutdown replaces it
    // with "Safe to unplug"). In a simulation or a replay it only quits the app.
    QObject::connect(&panel, &FrontPanel::shutdownRequested, &app, [&] {
        if (sim || !replay.isEmpty()) {
            QTimer::singleShot(1000, &app, &QCoreApplication::quit);
            return;
        }
        display->keepOnExit();
        radio.shutdown();   // unlink from the networks and save the settings now
        if (!QProcess::startDetached("/sbin/poweroff", {})) {
            radio.logEvent("ERROR", "Shutdown: cannot run /sbin/poweroff");
            panel.message("Shutdown failed", "Unplug with care", 10000);
        }
    });

    quitOnSignals();
    QObject::connect(&app, &QCoreApplication::aboutToQuit, &radio, &Radio::shutdown);
    return QCoreApplication::exec();
}
