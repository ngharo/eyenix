#include "models.h"

#include <QFileInfo>
#include <QStringList>

QString modelDirectory() {
    const QString binary = QFileInfo(QStringLiteral("/proc/self/exe")).canonicalPath();
    QStringList candidates;
    if (qEnvironmentVariableIsSet("EYENIX_MODEL_DIR"))
        candidates << qEnvironmentVariable("EYENIX_MODEL_DIR");
    candidates << binary + "/../models"                // build/eyenix in the source tree
               << binary + "/../share/eyenix/models"   // installed under any prefix
               << QStringLiteral("/usr/share/eyenix/models");
    for (const QString &dir : candidates) {
        if (QFileInfo::exists(dir + "/face_detector.onnx") && QFileInfo::exists(dir + "/face_landmarks_detector.onnx"))
            return QFileInfo(dir).canonicalFilePath();
    }
    return {};
}

QString bridgeExecutable() {
    const QString binary = QFileInfo(QStringLiteral("/proc/self/exe")).canonicalPath();
    QStringList candidates;
    if (qEnvironmentVariableIsSet("EYENIX_BRIDGE"))
        candidates << qEnvironmentVariable("EYENIX_BRIDGE");
    candidates << binary + "/eyenix-bridge.exe"
               << binary + "/../lib/eyenix/eyenix-bridge.exe"
               << QStringLiteral("/usr/lib/eyenix/eyenix-bridge.exe");
    for (const QString &path : candidates)
        if (QFileInfo(path).isFile())
            return QFileInfo(path).canonicalFilePath();
    return {};
}
