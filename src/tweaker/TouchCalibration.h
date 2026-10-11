// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "common/service/Logger.h"

namespace mod {

class TouchCalibration : public QObject, public Singleton<TouchCalibration>, private Logger {
    Q_OBJECT
    Q_PROPERTY(bool enabled READ enabled WRITE setEnabled NOTIFY stateChanged)
    Q_PROPERTY(bool supported READ supported NOTIFY stateChanged)

public:
    bool             enabled() const;
    bool             supported() const { return !mRule.isEmpty(); }
    void             setEnabled(bool value);
    Q_INVOKABLE void refresh();

signals:
    void stateChanged();

private:
    friend Singleton<TouchCalibration>;
    TouchCalibration();
    QByteArray mRule;
};

} // namespace mod
