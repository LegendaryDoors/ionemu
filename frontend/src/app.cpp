#include "app.h"
#include <QDir>
#include <QRegularExpression>
#include <QStandardPaths>

namespace app {

static QString ensure(const QString &path) {
    QDir().mkpath(path);
    return path;
}

QString settingsFile() { return ensure(QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation)) + "/" + kId + ".ini"; }
QString dataDir() { return ensure(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)); }
QString savesDir() { return ensure(dataDir() + "/saves"); }
QString statesDir() { return ensure(dataDir() + "/states"); }
QString cacheFile() { return dataDir() + "/gamelist.json"; }

QString safeFileName(const QString &s) {
    QString out = s;
    out.replace(QRegularExpression(R"([<>:"/\\|?*\x00-\x1f])"), "_");
    out = out.trimmed();
    while (out.endsWith('.')) out.chop(1);
    return out.isEmpty() ? QStringLiteral("disc") : out;
}

}  // namespace app
