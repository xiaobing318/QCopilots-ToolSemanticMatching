// SPDX-License-Identifier: MIT
#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QString>
#include <QUrl>
#include <atomic>
#include <functional>

namespace qctsm
{
struct ModelArtifact
{
  QString name;
  QUrl url;
  qint64 size = 0;
  QByteArray sha256;
};

// Blocking methods belong on a background thread. Each download creates its
// own Qt network event loop. Model weights are never included in the package.
class ModelFiles final
{
  public:
    using Progress = std::function<void(qint64 received, qint64 total)>;
    explicit ModelFiles(QString directory, ModelArtifact artifact = defaultArtifact());
    static QJsonObject manifest();
    static ModelArtifact defaultArtifact();
    QString filePath() const;
    bool verify(const std::atomic_bool *cancel = nullptr, QString *error = nullptr) const;
    bool importFile(const QString &source, const std::atomic_bool *cancel = nullptr,
                    QString *error = nullptr, const Progress &progress = {}) const;
    bool downloadMissing(const std::atomic_bool *cancel = nullptr,
                         QString *error = nullptr, const Progress &progress = {}) const;

  private:
    bool valid(QString *error) const;
    bool verifyPath(const QString &path, const std::atomic_bool *cancel, QString *error) const;
    bool checkSpace(QString *error) const;
    QString mDirectory;
    ModelArtifact mArtifact;
};
}
