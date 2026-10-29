// SPDX-License-Identifier: MIT
// Adapted from QCopilots-Search (MIT). See THIRD_PARTY_NOTICES.md.
#pragma once

#include <QString>
#include <QVector>
#include <optional>

namespace qctsm::detail {

enum class BackendMode { Auto, Cpu, Gpu };

std::optional<BackendMode> parseBackendMode(const QString& value, QString* error = nullptr);
QString backendModeName(BackendMode mode);
QString canonicalBackendName(const QString& registryName);

// A snapshot of real ggml enumeration at runtime. Tests exercise policy using
// descriptors only and never claim to execute GPU kernels.
struct BackendDevice {
    QString backend;
    QString name;
    QString description;
    bool cpu = false;
    bool gpu = false;
    // An explicit capability rejection, never an initialization failure.
    QString unavailableReason;
    // Upstream's hardware identity, normally a PCI bus ID. An empty ID may
    // only be replaced by a unique backend-local device name.
    QString deviceId;
    quint64 memoryTotal = 0;
    quint64 memoryFree = 0;
};

struct BackendDeviceSelector {
    QString backend;
    QString deviceId;
    QString name;
};

struct BackendSelection {
    int deviceIndex = -1;
    QString backend;
    QString fallbackReason;
    QString error;
    bool gpu = false;
};

BackendSelection selectBackend(BackendMode mode, const QString& packagedBackend,
                               const QVector<BackendDevice>& devices,
                               const QString& gpuLoadError = {});

QString deviceIdentityError(const BackendDevice& device, const QVector<BackendDevice>& devices);
QVector<int> compatibleGpuDevices(const QString& packagedBackend, const QVector<BackendDevice>& devices);
// Explicit selection is strict even in Auto mode. It must never silently use
// another device or CPU when an identity disappears or a backend fails.
BackendSelection selectBackendDevice(BackendMode mode, const QString& packagedBackend,
    const QVector<BackendDevice>& devices, const BackendDeviceSelector& selector,
    const QString& gpuLoadError = {});

}
