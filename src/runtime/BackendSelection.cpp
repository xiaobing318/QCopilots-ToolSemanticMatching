// SPDX-License-Identifier: MIT
// Adapted from QCopilots-Search (MIT). See THIRD_PARTY_NOTICES.md.
#include "BackendSelection.h"

#include <QStringList>

namespace qctsm::detail {

std::optional<BackendMode> parseBackendMode(const QString& value, QString* error)
{
    if (error) error->clear();
    const auto mode = value.trimmed().toLower();
    if (mode == QStringLiteral("auto")) return BackendMode::Auto;
    if (mode == QStringLiteral("cpu")) return BackendMode::Cpu;
    if (mode == QStringLiteral("gpu")) return BackendMode::Gpu;
    if (error) *error = QStringLiteral("Invalid inference backend mode '%1'. Expected auto, cpu or gpu.").arg(value);
    return std::nullopt;
}

QString backendModeName(BackendMode mode)
{
    switch (mode) {
    case BackendMode::Auto: return QStringLiteral("auto");
    case BackendMode::Cpu: return QStringLiteral("cpu");
    case BackendMode::Gpu: return QStringLiteral("gpu");
    }
    return {};
}

QString canonicalBackendName(const QString& registryName)
{
    const auto name = registryName.trimmed().toLower();
    // HIP and Metal registry names differ from their packaged backend names.
    if (name == QStringLiteral("rocm")) return QStringLiteral("hip");
    if (name == QStringLiteral("mtl")) return QStringLiteral("metal");
    return name;
}

QString deviceIdentityError(const BackendDevice& device, const QVector<BackendDevice>& devices)
{
    const auto backend = canonicalBackendName(device.backend);
    if (device.deviceId.isEmpty() && device.name.isEmpty())
        return QStringLiteral("The %1 device has neither a hardware ID nor a device name.").arg(backend);
    int matches = 0;
    for (const auto& candidate : devices) {
        if (canonicalBackendName(candidate.backend) != backend) continue;
        if (!device.deviceId.isEmpty() ? candidate.deviceId == device.deviceId : candidate.name == device.name)
            ++matches;
    }
    if (matches != 1)
        return QStringLiteral("The %1 device identity '%2' is not unique.")
            .arg(backend, device.deviceId.isEmpty() ? device.name : device.deviceId);
    return {};
}

QVector<int> compatibleGpuDevices(const QString& packagedBackend, const QVector<BackendDevice>& devices)
{
    const auto backend = canonicalBackendName(packagedBackend);
    QVector<int> result;
    if (backend != QStringLiteral("cuda") && backend != QStringLiteral("hip")
        && backend != QStringLiteral("vulkan") && backend != QStringLiteral("metal")) return result;
    for (qsizetype index = 0; index < devices.size(); ++index) {
        const auto& device = devices[index];
        if (device.gpu && canonicalBackendName(device.backend) == backend
            && !(backend == QStringLiteral("metal") && device.name.isEmpty())
            && device.unavailableReason.isEmpty() && deviceIdentityError(device, devices).isEmpty())
            result.append(int(index));
    }
    return result;
}

BackendSelection selectBackendDevice(BackendMode mode, const QString& packagedBackend,
    const QVector<BackendDevice>& devices, const BackendDeviceSelector& selector, const QString& gpuLoadError)
{
    // Reuse the package and CPU installation checks, independent of GPU policy.
    auto result = selectBackend(BackendMode::Cpu, packagedBackend, devices);
    if (!result.error.isEmpty()) return result;
    result = {};
    const auto requested = canonicalBackendName(selector.backend);
    const auto packaged = canonicalBackendName(packagedBackend);
    if (requested != QStringLiteral("cpu") && (requested != packaged || packaged == QStringLiteral("cpu"))) {
        result.error = QStringLiteral("Selected backend '%1' is not included in this %2 installation.").arg(requested, packaged);
        return result;
    }
    if ((mode == BackendMode::Gpu && requested == QStringLiteral("cpu"))
        || (mode == BackendMode::Cpu && requested != QStringLiteral("cpu"))) {
        result.error = QStringLiteral("Selected device conflicts with the requested %1 mode.").arg(backendModeName(mode));
        return result;
    }
    if (requested != QStringLiteral("cpu") && !gpuLoadError.isEmpty()) {
        result.error = gpuLoadError;
        return result;
    }
    if (selector.deviceId.isEmpty() && selector.name.isEmpty()) {
        result.error = QStringLiteral("Explicit device selection requires a hardware ID or a unique device name.");
        return result;
    }
    int match = -1;
    for (qsizetype index = 0; index < devices.size(); ++index) {
        const auto& device = devices[index];
        if (canonicalBackendName(device.backend) != requested
            || (requested == QStringLiteral("cpu") ? !device.cpu : !device.gpu)) continue;
        const bool matches = !selector.deviceId.isEmpty() ? device.deviceId == selector.deviceId
                                                         : device.name == selector.name;
        if (!matches) continue;
        if (match >= 0) {
            result.error = QStringLiteral("Selected device identity is ambiguous in backend %1.").arg(requested);
            return result;
        }
        match = int(index);
    }
    if (match < 0) {
        result.error = QStringLiteral("Selected %1 device '%2' is no longer available.")
            .arg(requested, selector.deviceId.isEmpty() ? selector.name : selector.deviceId);
        return result;
    }
    const auto& device = devices[match];
    if (selector.deviceId.isEmpty() && !device.deviceId.isEmpty()) {
        result.error = QStringLiteral("Selected device now supplies a hardware ID. Rediscover devices before binding it.");
        return result;
    }
    result.error = device.unavailableReason;
    if (result.error.isEmpty()) result.error = deviceIdentityError(device, devices);
    if (result.error.isEmpty() && requested == QStringLiteral("metal") && device.name.isEmpty())
        result.error = QStringLiteral("Selected Metal device has no physical device.");
    if (!result.error.isEmpty()) return result;
    result.deviceIndex = match;
    result.backend = requested;
    result.gpu = device.gpu;
    return result;
}

BackendSelection selectBackend(BackendMode mode, const QString& packagedBackend,
                               const QVector<BackendDevice>& devices, const QString& gpuLoadError)
{
    BackendSelection result;
    const auto packaged = canonicalBackendName(packagedBackend);
    if (packaged != QStringLiteral("cpu") && packaged != QStringLiteral("cuda")
        && packaged != QStringLiteral("hip") && packaged != QStringLiteral("vulkan")
        && packaged != QStringLiteral("metal")) {
        result.error = QStringLiteral("Unknown packaged inference backend '%1'.").arg(packagedBackend);
        return result;
    }
    int cpuIndex = -1;
    int gpuIndex = -1;
    QStringList unavailableReasons;
    for (qsizetype index = 0; index < devices.size(); ++index) {
        const auto& device = devices[index];
        const auto backend = canonicalBackendName(device.backend);
        // The pinned Metal backend returns an unnamed wrapper when no MTLDevice
        // is available. Real devices receive MTL<n> before backend initialization.
        const bool missingMetalDevice = backend == QStringLiteral("metal") && device.name.isEmpty();
        if (device.cpu && cpuIndex < 0) cpuIndex = int(index);
        if (device.gpu && !missingMetalDevice && packaged != QStringLiteral("cpu") && backend == packaged) {
            if (!device.unavailableReason.isEmpty()) unavailableReasons.append(device.unavailableReason);
            else if (gpuIndex < 0) gpuIndex = int(index);
        }
    }
    // ggml still needs its CPU backend for unsupported operations even when all
    // model layers are offloaded. A broken CPU installation is not a GPU fallback.
    if (cpuIndex < 0) {
        result.error = QStringLiteral("No compatible CPU device is available in the packaged runtime.");
        return result;
    }
    if (mode != BackendMode::Cpu && gpuIndex >= 0 && gpuLoadError.isEmpty()) {
        result.deviceIndex = gpuIndex;
        result.backend = packaged;
        result.gpu = true;
        return result;
    }
    QString reason;
    if (mode != BackendMode::Cpu) {
        if (packaged == QStringLiteral("cpu")) {
            if (mode == BackendMode::Gpu) reason = QStringLiteral("This installation includes CPU inference only.");
        } else {
            if (!gpuLoadError.isEmpty()) reason = gpuLoadError;
            else if (!unavailableReasons.isEmpty()) reason = unavailableReasons.join(QLatin1Char(' '));
            else reason = QStringLiteral("No compatible %1 GPU device was found. Check the installed GPU and driver.").arg(packaged);
        }
    }
    if (mode == BackendMode::Gpu) {
        result.error = QStringLiteral("Strict GPU mode cannot start: %1").arg(reason);
        return result;
    }
    result.deviceIndex = cpuIndex;
    result.backend = QStringLiteral("cpu");
    result.fallbackReason = reason;
    return result;
}

}
