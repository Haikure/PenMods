// SPDX-License-Identifier: GPL-3.0-only
/*
 * Copyright (C) 2022-present, PenUniverse.
 * This file is part of the PenMods open source project.
 */

#pragma once

#include "common/service/Logger.h"

namespace mod {

class TextBookHelper : public QObject, public Singleton<TextBookHelper>, private Logger {
    Q_OBJECT
    Q_PROPERTY(QString directory READ getRootPath CONSTANT)
    Q_PROPERTY(int installedCount READ installedCount NOTIFY changed)

public:
    Q_INVOKABLE void onUiCompleted();
    int              installedCount() const { return static_cast<int>(mBookIds.size()); }

    bool isInstalled(const QString&);

    QString getRootPath();

    void remove(const QString&);

signals:
    void changed();

private:
    friend Singleton<TextBookHelper>;
    explicit TextBookHelper();

    std::vector<QString> mBookIds;
};

} // namespace mod
