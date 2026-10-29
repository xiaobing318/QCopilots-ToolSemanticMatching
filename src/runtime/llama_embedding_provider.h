// SPDX-License-Identifier: MIT
#pragma once

#include "core/retrieval.h"
#include <QJsonObject>
#include <memory>

namespace qctsm
{
struct ModelOptions
{
  QString modelDirectory;
  QString backend = QStringLiteral("cpu");
  QString deviceId;
  QString deviceName;
  int threads = 0;
  int contextTokens = 2048;
};

// Owns the model/context on its calling worker thread. No text prefixes are
// inserted here: the shared retrieval layer owns query/document formatting.
class LlamaEmbeddingProvider final : public EmbeddingProvider
{
  public:
    explicit LlamaEmbeddingProvider(ModelOptions options);
    ~LlamaEmbeddingProvider() override;
    QList<QVector<float>> embed(const QStringList &texts, const std::atomic_bool *cancel,
                               QString *error) override;
    QJsonObject modelInfo() const;
    static QJsonObject devices(const QString &backend = QStringLiteral("auto"));

  private:
    class Impl;
    std::unique_ptr<Impl> d;
};
}
