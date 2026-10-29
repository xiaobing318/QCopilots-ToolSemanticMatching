// SPDX-License-Identifier: MIT
// Adapted from QCopilots-Search (MIT). See THIRD_PARTY_NOTICES.md.
#include "BackendLoader.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <mutex>
#include <vector>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace qctsm::detail {
namespace {
// The same manifest also inventories the architecture-specific kernel assets.
constexpr qsizetype ManifestLimit = 4 * 1024 * 1024;

QString resolveInstalledPath(const QString& value, const QString& root, bool directory, QString* error)
{
    QString relative = value;
    relative.replace(QLatin1Char('\\'), QLatin1Char('/'));
    if (directory && relative == QStringLiteral(".")) return root;
    const auto parts = relative.split(QLatin1Char('/'));
    if (relative.isEmpty() || relative.contains(QLatin1Char(':')) || relative.contains(QChar::Null)
        || relative.startsWith(QLatin1Char('/')) || QDir::isAbsolutePath(relative)
        || parts.contains(QString{}) || parts.contains(QStringLiteral(".")) || parts.contains(QStringLiteral(".."))) {
        *error = QStringLiteral("GPU runtime manifest contains an unsafe relative path: %1").arg(value);
        return {};
    }
    const QFileInfo info(QDir(root).filePath(relative));
    const auto resolved = info.canonicalFilePath();
#ifdef Q_OS_WIN
    constexpr auto comparison = Qt::CaseInsensitive;
#else
    constexpr auto comparison = Qt::CaseSensitive;
#endif
    const auto prefix = root.endsWith(QLatin1Char('/')) ? root : root + QLatin1Char('/');
    if (resolved.isEmpty() || !resolved.startsWith(prefix, comparison)
        || (directory ? !info.isDir() : !info.isFile())) {
        *error = QStringLiteral("GPU runtime path is missing, has the wrong type or escapes the installation: %1").arg(value);
        return {};
    }
    return resolved;
}
}

std::optional<GpuRuntimeManifest> parseGpuRuntimeManifest(const QByteArray& json,
    const QString& applicationDirectory, const QString& backend, QString* error)
{
    QString localError;
    if (!error) error = &localError;
    error->clear();
    if (json.size() > ManifestLimit) {
        *error = QStringLiteral("GPU runtime manifest exceeds 4 MiB.");
        return std::nullopt;
    }
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(json, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        *error = QStringLiteral("Cannot parse GPU runtime manifest: %1").arg(parseError.errorString());
        return std::nullopt;
    }
    const auto object = document.object();
    if (!object.value(QStringLiteral("schemaVersion")).isDouble()
        || object.value(QStringLiteral("schemaVersion")).toDouble() != 1
        || object.value(QStringLiteral("backend")).toString() != backend) {
        *error = QStringLiteral("GPU runtime manifest version or backend does not match this installation.");
        return std::nullopt;
    }
    const auto root = QFileInfo(applicationDirectory).canonicalFilePath();
    if (root.isEmpty() || !QFileInfo(root).isDir()) {
        *error = QStringLiteral("GPU runtime installation directory does not exist.");
        return std::nullopt;
    }
    GpuRuntimeManifest result;
    for (const auto& key : {QStringLiteral("libraryDirectories"), QStringLiteral("preloadLibraries")}) {
        const auto value = object.value(key);
        if (!value.isArray() || value.toArray().isEmpty() || value.toArray().size() > 64) {
            *error = QStringLiteral("GPU runtime manifest %1 must contain 1 to 64 paths.").arg(key);
            return std::nullopt;
        }
        const bool directory = key == QStringLiteral("libraryDirectories");
        auto& target = directory ? result.libraryDirectories : result.preloadLibraries;
        QSet<QString> seen;
        for (const auto& entry : value.toArray()) {
            if (!entry.isString()) {
                *error = QStringLiteral("GPU runtime manifest paths must be strings.");
                return std::nullopt;
            }
            const auto resolved = resolveInstalledPath(entry.toString(), root, directory, error);
            if (resolved.isEmpty()) return std::nullopt;
#ifdef Q_OS_WIN
            const auto identity = resolved.toLower();
#else
            const auto& identity = resolved;
#endif
            if (seen.contains(identity)) {
                *error = QStringLiteral("GPU runtime manifest contains a duplicate path: %1").arg(entry.toString());
                return std::nullopt;
            }
            seen.insert(identity);
            target.append(resolved);
        }
    }
    return result;
}

QString prepareGpuRuntime(const QString& applicationDirectory, const QString& backend)
{
#ifndef Q_OS_WIN
    // ELF/Mach-O runtime lookup is established by the installed relative RPATH.
    Q_UNUSED(applicationDirectory)
    Q_UNUSED(backend)
    return {};
#else
    const auto root = QFileInfo(applicationDirectory).canonicalFilePath();
    const auto manifestPath = QDir(applicationDirectory).filePath(QStringLiteral("qctsm-gpu-runtime.json"));
    if (!QFileInfo::exists(manifestPath)) {
        return backend == QStringLiteral("hip")
            ? QStringLiteral("Packaged ROCm runtime manifest qctsm-gpu-runtime.json is missing.") : QString{};
    }
    QString error;
    const auto safeManifest = resolveInstalledPath(QStringLiteral("qctsm-gpu-runtime.json"), root, false, &error);
    if (safeManifest.isEmpty()) return error;
    QFile file(safeManifest);
    if (!file.open(QIODevice::ReadOnly)) return QStringLiteral("Cannot open GPU runtime manifest: %1").arg(file.errorString());
    if (file.size() > ManifestLimit) return QStringLiteral("GPU runtime manifest exceeds 4 MiB.");
    const auto manifest = parseGpuRuntimeManifest(file.read(ManifestLimit + 1), applicationDirectory, backend, &error);
    if (!manifest) return error;

    static std::mutex mutex;
    std::lock_guard lock(mutex);
    // Do not unload these modules or remove the directories while ggml holds
    // function pointers into them. They are intentionally process-owned.
    static QSet<QString> registeredDirectories;
    static std::vector<void*> directoryCookies;
    static std::vector<HMODULE> modules;
    using AddDirectory = void* (WINAPI*)(PCWSTR);
    const auto addDirectory = reinterpret_cast<AddDirectory>(
        GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "AddDllDirectory"));
    if (!addDirectory) return QStringLiteral("Windows AddDllDirectory is unavailable for the GPU runtime.");
    for (const auto& directory : manifest->libraryDirectories) {
        const auto identity = directory.toLower();
        if (registeredDirectories.contains(identity)) continue;
        const auto native = QDir::toNativeSeparators(directory).toStdWString();
        auto cookie = addDirectory(native.c_str());
        if (!cookie) return QStringLiteral("Cannot register GPU runtime directory %1 (Windows error %2).")
            .arg(directory).arg(GetLastError());
        directoryCookies.push_back(cookie);
        registeredDirectories.insert(identity);
    }
    for (const auto& library : manifest->preloadLibraries) {
        if (QFileInfo(library).suffix().compare(QStringLiteral("dll"), Qt::CaseInsensitive) != 0)
            return QStringLiteral("GPU runtime preload is not a DLL: %1").arg(library);
        const auto name = QFileInfo(library).fileName().toStdWString();
        if (auto loaded = GetModuleHandleW(name.c_str())) {
            std::vector<wchar_t> buffer(32768);
            const auto length = GetModuleFileNameW(loaded, buffer.data(), DWORD(buffer.size()));
            const auto loadedPath = length > 0 && length < buffer.size()
                ? QFileInfo(QString::fromWCharArray(buffer.data(), int(length))).canonicalFilePath() : QString{};
            if (loadedPath.compare(library, Qt::CaseInsensitive) != 0)
                return QStringLiteral("GPU dependency %1 was already loaded from a different location: %2")
                    .arg(QFileInfo(library).fileName(), loadedPath);
            continue;
        }
        const auto native = QDir::toNativeSeparators(library).toStdWString();
        auto module = LoadLibraryExW(native.c_str(), nullptr,
            LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_USER_DIRS | LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!module) return QStringLiteral("Cannot preload packaged GPU dependency %1 (Windows error %2).")
            .arg(library).arg(GetLastError());
        modules.push_back(module);
    }
    return {};
#endif
}

}
