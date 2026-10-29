// SPDX-License-Identifier: MIT
#include "http_embedding_provider.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QTimer>

#include <cmath>
#include <memory>
#include <limits>

namespace qctsm
{
namespace
{
constexpr qsizetype MaxRequestBytes = 16 * 1024 * 1024;
constexpr qsizetype MaxResponseBytes = 64 * 1024 * 1024;
constexpr qsizetype MaxDimensions = 65536;

QList<QVector<float>> failure(QString *error, const QString &message)
{
  if (error)
    *error = message;
  return {};
}

bool isCancelled(const std::atomic_bool *cancel)
{
  return cancel && cancel->load(std::memory_order_relaxed);
}

bool transientNetworkError(QNetworkReply::NetworkError error)
{
  switch (error)
  {
    case QNetworkReply::ConnectionRefusedError:
    case QNetworkReply::RemoteHostClosedError:
    case QNetworkReply::HostNotFoundError:
    case QNetworkReply::TimeoutError:
    case QNetworkReply::TemporaryNetworkFailureError:
    case QNetworkReply::NetworkSessionFailedError:
    case QNetworkReply::ProxyConnectionRefusedError:
    case QNetworkReply::ProxyConnectionClosedError:
    case QNetworkReply::ProxyTimeoutError:
      return true;
    default:
      return false;
  }
}

QString serverError(const QByteArray &body)
{
  const QJsonObject object = QJsonDocument::fromJson(body).object();
  const QJsonValue error = object.value(QStringLiteral("error"));
  const QString message = error.isObject()
    ? error.toObject().value(QStringLiteral("message")).toString()
    : error.toString();
  return message.isEmpty() ? QString() : QStringLiteral(": ") + message.left(512);
}
}

HttpEmbeddingProvider::HttpEmbeddingProvider(const QUrl &apiBase,
                                             const QString &modelId,
                                             const QByteArray &authorization,
                                             int timeoutMs)
  : mApiBase(apiBase), mModelId(modelId), mAuthorization(authorization),
    mTimeoutMs(timeoutMs)
{
  QString path = mApiBase.path();
  if (!path.endsWith(QLatin1Char('/')))
    mApiBase.setPath(path + QLatin1Char('/'));
}

void HttpEmbeddingProvider::setRequestHeaders(const QMap<QByteArray, QByteArray> &headers)
{
  QWriteLocker locker(&mHeadersLock);
  mHeaders = headers;
}

QList<QVector<float>> HttpEmbeddingProvider::embed(const QStringList &texts,
                                                 const std::atomic_bool *cancel,
                                                 QString *error)
{
  QElapsedTimer overall;
  overall.start();
  QMap<QByteArray, QByteArray> headers;
  {
    QReadLocker locker(&mHeadersLock);
    headers = mHeaders;
  }
  QString attemptError;
  for (int attempt = 0; attempt < 3; ++attempt)
  {
    const int remaining = mTimeoutMs - static_cast<int>(overall.elapsed());
    if (attempt > 0 && remaining <= 0)
      return failure(error, QStringLiteral("timeout: Embedding retries exceeded the overall deadline."));
    bool retryable = false;
    int retryAfterMs = 0;
    auto vectors = embedOnce(texts, cancel, &attemptError, headers, remaining,
                             &retryable, &retryAfterMs);
    if (!retryable || attempt == 2)
    {
      if (error)
        *error = attemptError;
      return vectors;
    }
    const int delayMs = qMax(attempt == 0 ? 200 : 500, retryAfterMs);
    if (overall.elapsed() + delayMs >= mTimeoutMs)
      return failure(error, attemptError); // Respect Retry-After without extending the deadline.
    QEventLoop backoff;
    QTimer delay;
    delay.setSingleShot(true);
    QTimer cancellation;
    cancellation.setInterval(25);
    QObject::connect(&delay, &QTimer::timeout, &backoff, &QEventLoop::quit);
    QObject::connect(&cancellation, &QTimer::timeout, &backoff, [&]() {
      if (isCancelled(cancel))
        backoff.quit();
    });
    delay.start(delayMs);
    if (cancel)
      cancellation.start();
    if (!isCancelled(cancel))
      backoff.exec();
    if (isCancelled(cancel))
      return failure(error, QStringLiteral("cancelled: Embedding retry cancelled."));
  }
  return failure(error, attemptError);
}

QList<QVector<float>> HttpEmbeddingProvider::embedOnce(const QStringList &texts,
                                                     const std::atomic_bool *cancel,
                                                     QString *error,
                                                     const QMap<QByteArray, QByteArray> &headers,
                                                     int timeoutMs,
                                                     bool *retryable,
                                                     int *retryAfterMs)
{
  if (error)
    error->clear();
  if (isCancelled(cancel))
    return failure(error, QStringLiteral("cancelled: Embedding request cancelled."));
  if (texts.isEmpty())
    return {};
  if (!QCoreApplication::instance())
    return failure(error, QStringLiteral("missing_application: A Qt application is required."));
  if (!mApiBase.isValid() || mApiBase.host().isEmpty()
      || (mApiBase.scheme() != QLatin1String("http") && mApiBase.scheme() != QLatin1String("https"))
      || !mApiBase.userInfo().isEmpty() || mApiBase.hasQuery() || mApiBase.hasFragment())
    return failure(error, QStringLiteral("invalid_api_base: Expected an HTTP(S) API base without userinfo, query or fragment."));
  if (mModelId.trimmed().isEmpty() || timeoutMs <= 0)
    return failure(error, QStringLiteral("invalid_configuration: Model ID and a positive timeout are required."));
  if (mAuthorization.contains('\r') || mAuthorization.contains('\n'))
    return failure(error, QStringLiteral("invalid_authorization: Header contains a newline."));
  static const QRegularExpression headerName(QStringLiteral("^[!#$%&'*+.^_`|~0-9A-Za-z-]+$"));
  for (auto it = headers.cbegin(); it != headers.cend(); ++it)
  {
    const QByteArray lower = it.key().toLower();
    if (!headerName.match(QString::fromLatin1(it.key())).hasMatch()
        || it.value().contains('\r') || it.value().contains('\n')
        || lower == "host" || lower == "content-length" || lower == "transfer-encoding"
        || lower == "connection")
      return failure(error, QStringLiteral("invalid_headers: Invalid authentication header name or value."));
  }

  QJsonArray input;
  for (const QString &text : texts)
    input.append(text);
  const QByteArray payload = QJsonDocument(QJsonObject{
    {QStringLiteral("model"), mModelId},
    {QStringLiteral("input"), input},
    {QStringLiteral("encoding_format"), QStringLiteral("float")}
  }).toJson(QJsonDocument::Compact);
  if (payload.size() > MaxRequestBytes)
    return failure(error, QStringLiteral("request_too_large: Embedding batch exceeds 16 MiB."));

  QNetworkAccessManager manager;
  QNetworkRequest request(mApiBase.resolved(QUrl(QStringLiteral("embeddings"))));
  request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json; charset=utf-8"));
  request.setRawHeader("Accept", "application/json");
  if (!mAuthorization.isEmpty())
    request.setRawHeader("Authorization", mAuthorization);
  for (auto it = headers.cbegin(); it != headers.cend(); ++it)
    request.setRawHeader(it.key(), it.value());
  // Never forward authorization to a different host after a redirect.
  request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                       QNetworkRequest::SameOriginRedirectPolicy);

  QEventLoop loop;
  std::unique_ptr<QNetworkReply> reply(manager.post(request, payload));
  reply->setReadBufferSize(MaxResponseBytes + 1);
  QTimer deadline;
  deadline.setSingleShot(true);
  QTimer cancellation;
  cancellation.setInterval(25);
  QByteArray response;
  bool timedOut = false;
  bool cancelled = false;
  bool tooLarge = false;
  const auto readAvailable = [&]() {
    if (!reply->isOpen())
      return;
    const qint64 remaining = static_cast<qint64>(MaxResponseBytes - response.size());
    response.append(reply->read(remaining + 1));
    if (response.size() > MaxResponseBytes)
    {
      tooLarge = true;
      reply->abort();
    }
  };
  QObject::connect(reply.get(), &QIODevice::readyRead, &loop, readAvailable);
  QObject::connect(reply.get(), &QNetworkReply::finished, &loop, &QEventLoop::quit);
  QObject::connect(&deadline, &QTimer::timeout, &loop, [&]() {
    timedOut = true;
    reply->abort();
    loop.quit();
  });
  QObject::connect(&cancellation, &QTimer::timeout, &loop, [&]() {
    if (isCancelled(cancel))
    {
      cancelled = true;
      reply->abort();
      loop.quit();
    }
  });
  deadline.start(timeoutMs);
  if (cancel)
    cancellation.start();
  if (!reply->isFinished())
    loop.exec();
  deadline.stop();
  cancellation.stop();
  if (!tooLarge)
    readAvailable();
  if (cancelled || isCancelled(cancel))
    return failure(error, QStringLiteral("cancelled: Embedding request cancelled."));
  if (timedOut)
    return failure(error, QStringLiteral("timeout: Embedding request exceeded its deadline."));
  if (tooLarge)
    return failure(error, QStringLiteral("response_too_large: Embedding response exceeds 64 MiB."));
  const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
  if (status && (status < 200 || status >= 300))
  {
    *retryable = status == 429 || status == 502 || status == 503 || status == 504;
    bool retrySecondsValid = false;
    const qint64 retrySeconds = reply->rawHeader("Retry-After").toLongLong(&retrySecondsValid);
    if (retrySecondsValid && retrySeconds > 0)
      *retryAfterMs = static_cast<int>(qMin(retrySeconds,
        static_cast<qint64>(std::numeric_limits<int>::max() / 1000)) * 1000);
    return failure(error, QStringLiteral("http_%1: Embedding endpoint rejected the request%2")
                            .arg(status).arg(serverError(response)));
  }
  if (reply->error() != QNetworkReply::NoError)
  {
    *retryable = transientNetworkError(reply->error());
    return failure(error, QStringLiteral("network_error: ") + reply->errorString());
  }
  if (!status)
    return failure(error, QStringLiteral("invalid_response: No HTTP status was returned."));

  QJsonParseError parseError;
  const QJsonDocument document = QJsonDocument::fromJson(response, &parseError);
  if (parseError.error != QJsonParseError::NoError || !document.isObject())
    return failure(error, QStringLiteral("invalid_json: Embedding endpoint did not return a JSON object."));
  const QJsonValue dataValue = document.object().value(QStringLiteral("data"));
  if (!dataValue.isArray() || dataValue.toArray().size() != texts.size())
    return failure(error, QStringLiteral("invalid_batch: Embedding response count differs from input count."));

  QList<QVector<float>> vectors;
  vectors.resize(texts.size());
  QVector<bool> seen(texts.size(), false);
  qsizetype dimensions = -1;
  for (const QJsonValue &entryValue : dataValue.toArray())
  {
    if (isCancelled(cancel))
      return failure(error, QStringLiteral("cancelled: Embedding response validation cancelled."));
    if (!entryValue.isObject())
      return failure(error, QStringLiteral("invalid_batch: Embedding item is not an object."));
    const QJsonObject entry = entryValue.toObject();
    const QJsonValue indexValue = entry.value(QStringLiteral("index"));
    const double indexNumber = indexValue.toDouble(-1.0);
    if (!indexValue.isDouble() || !std::isfinite(indexNumber) || indexNumber < 0
        || indexNumber >= texts.size() || std::floor(indexNumber) != indexNumber)
      return failure(error, QStringLiteral("invalid_index: Embedding response index is missing or out of range."));
    const qsizetype index = static_cast<qsizetype>(indexNumber);
    if (seen[index])
      return failure(error, QStringLiteral("duplicate_index: Embedding response repeats an input index."));
    seen[index] = true;
    const QJsonValue vectorValue = entry.value(QStringLiteral("embedding"));
    if (!vectorValue.isArray())
      return failure(error, QStringLiteral("invalid_vector: Expected a float embedding array."));
    const QJsonArray values = vectorValue.toArray();
    if (values.isEmpty() || values.size() > MaxDimensions
        || (dimensions >= 0 && dimensions != values.size()))
      return failure(error, QStringLiteral("invalid_dimensions: Embedding vectors have empty, excessive or inconsistent dimensions."));
    dimensions = values.size();
    QVector<float> vector;
    vector.reserve(dimensions);
    double normSquared = 0.0;
    for (const QJsonValue &value : values)
    {
      const double number = value.toDouble();
      const float narrowed = static_cast<float>(number);
      if (!value.isDouble() || !std::isfinite(number) || !std::isfinite(narrowed))
        return failure(error, QStringLiteral("invalid_vector: Embedding contains a non-finite or non-numeric element."));
      vector.append(narrowed);
      normSquared += static_cast<double>(narrowed) * narrowed;
    }
    if (!std::isfinite(normSquared) || normSquared <= 0.0)
      return failure(error, QStringLiteral("zero_vector: Embedding has no finite non-zero norm."));
    vectors[index] = std::move(vector);
  }
  return vectors;
}
}
