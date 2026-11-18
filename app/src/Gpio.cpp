#include "Gpio.h"
#include "gpio_v2.h"

#include <QDir>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

namespace Gpio {

QString chip()
{
    static QString found;
    if (!found.isEmpty()) return found;
    found = "/dev/gpiochip0";
    for (const QString &name : QDir("/dev").entryList({"gpiochip*"}, QDir::System)) {
        const QString path = "/dev/" + name;
        const int fd = ::open(path.toLatin1().constData(), O_RDONLY | O_CLOEXEC);
        if (fd < 0) continue;
        gpiochip_info info{};
        const bool ok = ioctl(fd, GPIO_GET_CHIPINFO_IOCTL, &info) == 0;
        ::close(fd);
        if (ok && (strstr(info.label, "bcm2711") || strstr(info.label, "bcm2835"))) { found = path; break; }
    }
    return found;
}

static int request(const QList<unsigned> &gpios, uint64_t flags, unsigned debounceUs, QString *err)
{
    const QString path = chip();
    const int chipFd = ::open(path.toLatin1().constData(), O_RDONLY | O_CLOEXEC);
    if (chipFd < 0) { if (err) *err = "cannot open " + path + ": " + strerror(errno); return -1; }
    gpio_v2_line_request req{};
    for (int i = 0; i < gpios.size(); ++i) req.offsets[i] = gpios[i];
    req.num_lines = __u32(gpios.size());
    strncpy(req.consumer, "digital-transceiver", sizeof req.consumer - 1);
    req.config.flags = flags;
    if (debounceUs) {
        req.config.num_attrs = 1;
        req.config.attrs[0].attr.id = GPIO_V2_LINE_ATTR_ID_DEBOUNCE;
        req.config.attrs[0].attr.debounce_period_us = debounceUs;
        req.config.attrs[0].mask = (1ULL << gpios.size()) - 1;
    }
    req.event_buffer_size = 64;
    const int rc = ioctl(chipFd, GPIO_V2_GET_LINE_IOCTL, &req);
    const int e = errno;
    ::close(chipFd);
    if (rc < 0) {
        QStringList names;
        for (unsigned g : gpios) names << QString::number(g);
        if (err) *err = QString("cannot claim GPIO %1 on %2: %3").arg(names.join(','), path, strerror(e));
        return -1;
    }
    return req.fd;
}

int requestInputs(const QList<unsigned> &gpios, unsigned debounceUs, QString *err)
{
    return request(gpios, GPIO_V2_LINE_FLAG_INPUT | GPIO_V2_LINE_FLAG_EDGE_RISING | GPIO_V2_LINE_FLAG_EDGE_FALLING |
                              GPIO_V2_LINE_FLAG_BIAS_PULL_UP, debounceUs, err);
}

int requestOutputs(const QList<unsigned> &gpios, QString *err)
{
    return request(gpios, GPIO_V2_LINE_FLAG_OUTPUT, 0, err);   // outputs start low
}

uint64_t values(int fd, int count)
{
    gpio_v2_line_values v{};
    v.mask = (1ULL << count) - 1;
    return ioctl(fd, GPIO_V2_LINE_GET_VALUES_IOCTL, &v) == 0 ? v.bits : 0;
}

bool setValues(int fd, uint64_t bits, uint64_t mask)
{
    gpio_v2_line_values v{};
    v.bits = bits;
    v.mask = mask;
    return ioctl(fd, GPIO_V2_LINE_SET_VALUES_IOCTL, &v) == 0;
}

}
