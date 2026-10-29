// SPDX-License-Identifier: MIT
// Adapted from QCopilots-Search (MIT). See THIRD_PARTY_NOTICES.md.
#pragma once

#include <QDir>
#include <QStringList>

namespace qctsm::detail {

enum class BackendPlatform { Windows, MacOS, Unix };

inline QStringList cpuBackendSearchRoots(const QString& applicationDirectory, BackendPlatform platform)
{
    const QDir bin(applicationDirectory);
    if (platform == BackendPlatform::MacOS && bin.dirName() == QStringLiteral("MacOS")
        && QDir(QDir::cleanPath(bin.absoluteFilePath(QStringLiteral("..")))).dirName() == QStringLiteral("Contents")) {
        // macdeployqt signs loadable modules under the bundle's PlugIns tree.
        // Do not fall back to obsolete, potentially unsigned Frameworks copies.
        return {QDir::cleanPath(bin.absoluteFilePath(QStringLiteral("../PlugIns/ggml")))};
    }
    // Standalone developer binaries and the Windows/Linux install layouts.
    return {bin.absolutePath(), bin.filePath(QStringLiteral("../lib")),
            bin.filePath(QStringLiteral("../Frameworks"))};
}

}
