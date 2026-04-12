#pragma once
#include "PluginInfo.h"
#include "common/service/Logger.h"
#include "common/service/Singleton.h"
#include <QLibrary>
#include <QList>
#include <QMap>
#include <QObject>
#include <QQmlEngine>

namespace mod {

class PluginManager : public QObject, public Singleton<PluginManager>, private Logger {
    Q_OBJECT
public:
    explicit PluginManager();
    ~PluginManager();

    void                     scanAndLoadAll();
    Q_INVOKABLE bool         togglePlugin(QString pluginId, bool enable);
    Q_INVOKABLE bool         uninstallPlugin(QString pluginId);
    const QList<PluginInfo>& getPlugins() const { return m_plugins; }

    // 设置 QML 引擎（在 UI 初始化时调用）
    void        setEngine(QQmlEngine* engine);
    QQmlEngine* engine() const { return m_engine; }

    // 获取特定插件的 QML 入口路径
    QString getPluginMainQml(const QString& pluginId);

signals:
    void pluginsChanged();

private:
    const QString            m_defaultDir = "/userdisk/PenMods/plugins";
    QList<PluginInfo>        m_plugins;
    QMap<QString, QLibrary*> m_loadedLibraries;
    QQmlEngine*              m_engine   = nullptr;
    bool                     m_scanning = false;

    bool parseMetadata(const QString& path, PluginInfo& info);
    bool loadSo(PluginInfo& info);
    void setPluginPersistence(const PluginInfo& info, bool enable);

    // 对已加载但未初始化引擎的插件，补充调用 attach
    void attachEngineToLoadedPlugins();
    
    // 初始化插件的 Hook API
    void initializePluginHookAPI(const PluginInfo& info);

    friend class Singleton<PluginManager>;
};

} // namespace mod
