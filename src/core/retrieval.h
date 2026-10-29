// SPDX-License-Identifier: MIT
#pragma once

#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>
#include <QVector>

#include <atomic>
#include <functional>
#include <memory>

namespace qctsm
{
struct EmbeddingProfile
{
  QString modelId = QStringLiteral("embeddinggemma-2-Q8_0");
  QString revision = QStringLiteral("bfcd298762cc34d0357ece5ebdd31791a3a374d8");
  int dimensions = 768;

  QString fingerprint() const;
  QJsonObject toJson() const;
  static EmbeddingProfile fromJson(const QJsonObject &object);
};

class EmbeddingProvider
{
  public:
    virtual ~EmbeddingProvider() = default;
    virtual QList<QVector<float>> embed(const QStringList &texts,
                                       const std::atomic_bool *cancel,
                                       QString *error) = 0;
};

class RetrievalEngine final
{
  public:
    RetrievalEngine(const QString &databasePath,
                    std::shared_ptr<EmbeddingProvider> provider,
                    const EmbeddingProfile &profile = {});
    ~RetrievalEngine();
    RetrievalEngine(const RetrievalEngine &) = delete;
    RetrievalEngine &operator=(const RetrievalEngine &) = delete;

    QJsonObject syncCatalog(const QJsonObject &snapshot,
                            const std::atomic_bool *cancel = nullptr,
                            const std::function<void(qint64 completed, qint64 total)> &progress = {});
    QJsonObject search(const QJsonObject &request,
                       const std::atomic_bool *cancel = nullptr);
    QJsonObject getRecords(const QStringList &ids);
    QJsonObject status();

  private:
    class Impl;
    std::unique_ptr<Impl> d;
};
}
