#pragma once

#include "PluginManager.h"
#include "common/service/Singleton.h"
#include <QJSEngine>
#include <QObject>
#include <QQmlEngine>

namespace mod {

/**
 * @brief 为 QML 提供插件管理功能的包装类
 */
class QmlPluginWrapper : public QObject, public Singleton<QmlPluginWrapper> {
    Q_OBJECT
    QML_SINGLETON
    QML_NAMED_ELEMENT(PluginManager)

public:
    explicit QmlPluginWrapper(QObject* parent = nullptr);

    Q_INVOKABLE int         getPluginCount();
    Q_INVOKABLE QJsonObject getPluginInfo(int index);
    Q_INVOKABLE bool        setPluginEnabled(const QString& pluginName, bool enabled);
    Q_INVOKABLE void        uninstallPlugin(const QString& pluginName);
    Q_INVOKABLE void        requestPluginList();

signals:
    void pluginListUpdated();
    void pluginStateChanged(const QString& pluginName, bool newState);

private slots:
    void onPluginsChanged();

private:
    PluginManager* m_pluginManager;
    QString        resolvePluginId(const QString& idOrName) const;
    friend class Singleton<QmlPluginWrapper>;
};

} // namespace mod
