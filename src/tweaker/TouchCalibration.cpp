// SPDX-License-Identifier: GPL-3.0-only
#include "tweaker/TouchCalibration.h"
#include "common/Event.h"
#include "common/Utils.h"

#include <QDir>
#include <QFile>
#include <QQmlContext>
#include <QSaveFile>
#include <fcntl.h>
#include <linux/input.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace mod {
namespace {
constexpr auto RULE_PATH = "/etc/udev/rules.d/99-penmods-touch-calibration.rules";
}

TouchCalibration::TouchCalibration() : Logger("TouchCalibration") {
    connect(&Event::getInstance(), &Event::beforeUiInitialization, [this](QQuickView&, QQmlContext* context) {
        refresh();
        context->setContextProperty("touchCalibration", this);
    });
}

bool TouchCalibration::enabled() const { return QFile::exists(RULE_PATH); }

void TouchCalibration::refresh() {
    mRule.clear();
    for (const auto& file : QDir("/dev/input").entryInfoList({"event*"}, QDir::System | QDir::Files)) {
        const int fd = ::open(file.absoluteFilePath().toLocal8Bit().constData(), O_RDONLY | O_CLOEXEC);
        if (fd < 0) continue;
        char          name[256] = {};
        input_absinfo x{}, y{};
        unsigned long properties = 0;
        const bool    valid      = ioctl(fd, EVIOCGNAME(sizeof(name) - 1), name) > 0
                        && ioctl(fd, EVIOCGPROP(sizeof(properties)), &properties) >= 0
                        && (properties & (1UL << INPUT_PROP_DIRECT)) && ioctl(fd, EVIOCGABS(ABS_MT_POSITION_X), &x) == 0
                        && ioctl(fd, EVIOCGABS(ABS_MT_POSITION_Y), &y) == 0;
        ::close(fd);
        if (!valid || x.maximum <= x.minimum || y.maximum <= y.minimum) continue;
        const QByteArray device(name);
        if (device.contains('"') || device.contains('\\') || device.contains('\n')) continue;
        // Weston rotates both pen panels; compensate one raw-Y unit without
        // copying YDP02X's fixed 1/320 matrix onto the wider YDP03X panel.
        const auto offset = QByteArray::number(1.0 / (y.maximum - y.minimum), 'g', 10);
        mRule = "ATTRS{name}==\"" + device + "\", ENV{LIBINPUT_CALIBRATION_MATRIX}=\"1 0 0 0 1 " + offset + "\"\n";
        info("Touch calibration: {} range {}..{} / {}..{}", name, x.minimum, x.maximum, y.minimum, y.maximum);
        break;
    }
    emit stateChanged();
}

void TouchCalibration::setEnabled(bool value) {
    if (value == enabled()) return;
    if (value) {
        refresh();
        if (!supported()) {
            showToast("未检测到可校准的触摸屏", "#E9900C");
            return;
        }
        QDir().mkpath("/etc/udev/rules.d");
        QSaveFile file(RULE_PATH);
        if (!file.open(QIODevice::WriteOnly) || file.write(mRule) != mRule.size() || !file.commit()) {
            showToast("无法保存触摸校准设置", "#E9900C");
            return;
        }
    } else if (!QFile::remove(RULE_PATH)) {
        showToast("无法关闭触摸校准", "#E9900C");
        return;
    }
    emit stateChanged();
    exec("sync; (sleep 1; reboot) >/dev/null 2>&1 &");
}

} // namespace mod
