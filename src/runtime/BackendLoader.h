// SPDX-License-Identifier: MIT
// Adapted from QCopilots-Search (MIT). See THIRD_PARTY_NOTICES.md.
#pragma once

#include <QByteArray>
#include <QStringList>
#include <optional>

namespace qctsm::detail {

struct GpuRuntimeManifest {
    QStringList libraryDirectories;
    QStringList preloadLibraries;
};

// Resolve only existing paths contained by the installed bin directory,
// including symlink resolution. Returned paths are absolute and canonical.
std::optional<GpuRuntimeManifest> parseGpuRuntimeManifest(const QByteArray& json,
    const QString& applicationDirectory, const QString& backend, QString* error = nullptr);

// Empty return means ready. Windows HIP packages must carry the manifest.
// Runtime modules and DLL directory registrations remain alive for the process.
QString prepareGpuRuntime(const QString& applicationDirectory, const QString& backend);

}
