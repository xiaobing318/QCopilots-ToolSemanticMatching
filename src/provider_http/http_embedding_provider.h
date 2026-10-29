// SPDX-License-Identifier: MIT
#pragma once

#include "core/retrieval.h"

#include <QByteArray>
#include <QMap>
#include <QReadWriteLock>
#include <QUrl>

namespace qctsm
{
/** A thread-independent configuration for OpenAI-compatible embedding requests.
 * Each embed call owns its network manager and event loop. Call embed from a
 * worker thread rather than a GUI thread. Query/document prefixes are owned by
 * the retrieval engine and are never added by this provider.
 */
class HttpEmbeddingProvider final : public EmbeddingProvider
{
  public:
    HttpEmbeddingProvider(const QUrl &apiBase,
                          const QString &modelId,
                          const QByteArray &authorization = {},
                          int timeoutMs = 60000);

    // Set before submission. Each embed call takes an independent snapshot.
    void setRequestHeaders(const QMap<QByteArray, QByteArray> &headers);

    QList<QVector<float>> embed(const QStringList &texts,
                               const std::atomic_bool *cancel,
                               QString *error) override;

  private:
    QList<QVector<float>> embedOnce(const QStringList &texts,
                                   const std::atomic_bool *cancel,
                                   QString *error,
                                   const QMap<QByteArray, QByteArray> &headers,
                                   int timeoutMs,
                                   bool *retryable,
                                   int *retryAfterMs);
    QUrl mApiBase;
    QString mModelId;
    QByteArray mAuthorization;
    int mTimeoutMs;
    QReadWriteLock mHeadersLock;
    QMap<QByteArray, QByteArray> mHeaders;
};
}
