#include "QmlPluginWrapper.h"
#include <QJsonArray>
#include <QUrl>

namespace mod {

QmlPluginWrapper::QmlPluginWrapper(QObject* parent) : QObject(parent), m_pluginManager(&PluginManager::getInstance()) {
    // 连接底层插件管理器的信号
    connect(m_pluginManager, &PluginManager::pluginsChanged, this, &QmlPluginWrapper::onPluginsChanged);
}

int QmlPluginWrapper::getPluginCount() {
    auto& plugins = m_pluginManager->getPlugins();
    return plugins.size();
}

QJsonObject QmlPluginWrapper::getPluginInfo(int index) {
    auto& plugins = m_pluginManager->getPlugins();
    if (index < 0 || index >= plugins.size()) return QJsonObject();

    const auto& plugin = plugins[index];
    QJsonObject obj;
    obj["id"]          = plugin.id; // 必须传递 ID
    obj["name"]        = plugin.name;
    obj["description"] = plugin.description;
    obj["version"]     = plugin.version;
    obj["author"]      = plugin.author;
    // UI 开关状态绑定到 isEnabled (偏好)，但如果 isLoaded 为 false 且 isEnabled 为 true，UI 应该知道出错了
    obj["enabled"] = plugin.isEnabled;
    obj["loaded"]  = plugin.isLoaded;
    obj["icon"]    = plugin.icon.isEmpty() ? "settings/plugin" : plugin.icon;

    // 转换路径为 URL 格式供 QML 使用
    obj["mainQmlUrl"] = QUrl::fromLocalFile(plugin.mainQml).toString();

    return obj;
}

bool QmlPluginWrapper::setPluginEnabled(const QString& pluginId, bool enabled) {
    bool result = m_pluginManager->togglePlugin(resolvePluginId(pluginId), enabled);
    if (result) emit pluginListUpdated();
    return result;
}

void QmlPluginWrapper::uninstallPlugin(const QString& pluginName) {
    bool result = m_pluginManager->uninstallPlugin(resolvePluginId(pluginName));
    if (result) emit pluginListUpdated();
}

void QmlPluginWrapper::requestPluginList() {
    m_pluginManager->scanAndLoadAll();
}

void QmlPluginWrapper::onPluginsChanged() {
    emit pluginListUpdated();
}

QString QmlPluginWrapper::resolvePluginId(const QString& idOrName) const {
    const auto& plugins = m_pluginManager->getPlugins();
    for (const auto& p : plugins)
        if (p.id == idOrName) return idOrName;
    for (const auto& p : plugins)
        if (p.name == idOrName) return p.id;
    return idOrName;
}

} // namespace mod