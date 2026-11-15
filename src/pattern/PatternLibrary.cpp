#include "pattern/PatternLibrary.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>

namespace geck::pattern {

QString PatternLibrary::rootDir() {
    // Mirror Settings' location convention: <ConfigLocation>/gecko/<...>.
    const QString configPath = QStandardPaths::writableLocation(QStandardPaths::ConfigLocation);
    const QString patternsPath = QDir(configPath).filePath(QStringLiteral("gecko/patterns"));
    QDir().mkpath(patternsPath);
    return patternsPath;
}

QString PatternLibrary::customDir() {
    const QString customPath = QDir(rootDir()).filePath(QStringLiteral("Custom"));
    QDir().mkpath(customPath);
    return customPath;
}

QString PatternLibrary::bundledDir() {
    const QDir app(QCoreApplication::applicationDirPath());
    for (const QString& candidate : { QStringLiteral("stamps"), QStringLiteral("resources/stamps"),
             QStringLiteral("../Resources/resources/stamps") }) {
        const QFileInfo folder(app.filePath(candidate));
        if (folder.isDir()) {
            return folder.canonicalFilePath();
        }
    }
    return {};
}

} // namespace geck::pattern
