#pragma once
// Requests for the Pi's GPIO lines through the GPIO character device (see gpio_v2.h).
#include <QList>
#include <QString>
#include <cstdint>

namespace Gpio {
QString chip();   // the Pi's own GPIO controller, e.g. /dev/gpiochip0

// Inputs with pull-ups that report both edges (debounced by the kernel if debounceUs > 0).
// Returns the request's file descriptor, or -1 with err.
int requestInputs(const QList<unsigned> &gpios, unsigned debounceUs, QString *err);
// Outputs, all low to start.
int requestOutputs(const QList<unsigned> &gpios, QString *err);

uint64_t values(int fd, int count);                    // bit i = level of the request's line i
bool setValues(int fd, uint64_t bits, uint64_t mask);
}
