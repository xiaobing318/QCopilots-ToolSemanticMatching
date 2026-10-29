// SPDX-License-Identifier: MIT
#include "model_files.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLockFile>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStorageInfo>
#include <QTimer>
#include <limits>
#include <mutex>
#ifdef Q_OS_MACOS
#include <sys/mount.h>
#endif

static void initializeModelResources()
{
  Q_INIT_RESOURCE(qctsm_models);
}

namespace qctsm
{
namespace
{
constexpr qint64 chunkSize = 1024 * 1024;
bool fail(QString *error, const QString &message)
{
  if (error) *error = message;
  return false;
}
bool cancelled(const std::atomic_bool *cancel)
{
  return cancel && cancel->load(std::memory_order_relaxed);
}
QString lockPath(const QString &path)
{
#ifdef Q_OS_WIN
  // Qt 6.12's lock removal needs the same extended path used at creation.
  auto native = QDir::toNativeSeparators(QFileInfo(path).absoluteFilePath());
  if (native.startsWith(QStringLiteral("\\\\?\\")) || native.startsWith(QStringLiteral("\\\\.\\"))) return native;
  return native.startsWith(QStringLiteral("\\\\"))
    ? QStringLiteral("\\\\?\\UNC\\") + native.mid(2) : QStringLiteral("\\\\?\\") + native;
#else
  return path;
#endif
}
}

ModelFiles::ModelFiles(QString directory, ModelArtifact artifact)
  : mDirectory(directory.isEmpty() ? QString{} : QDir(directory).absolutePath()),
    mArtifact(std::move(artifact)) {}

QJsonObject ModelFiles::manifest()
{
  static std::once_flag once;
  std::call_once(once, initializeModelResources);
  QFile file(QStringLiteral(":/qctsm/qctsm-models.lock.json"));
  if (!file.open(QIODevice::ReadOnly)) return {};
  return QJsonDocument::fromJson(file.readAll()).object();
}

ModelArtifact ModelFiles::defaultArtifact()
{
  const auto models = manifest().value(QStringLiteral("models")).toArray();
  if (models.isEmpty()) return {};
  const auto artifacts = models.at(0).toObject().value(QStringLiteral("artifacts")).toArray();
  if (artifacts.isEmpty()) return {};
  const auto item = artifacts.at(0).toObject();
  return {item.value(QStringLiteral("name")).toString(), QUrl(item.value(QStringLiteral("url")).toString()),
          item.value(QStringLiteral("size")).toInteger(), item.value(QStringLiteral("sha256")).toString().toLatin1()};
}

QString ModelFiles::filePath() const { return QDir(mDirectory).filePath(mArtifact.name); }

bool ModelFiles::valid(QString *error) const
{
  static const QRegularExpression filePattern(QStringLiteral("^[A-Za-z0-9][A-Za-z0-9_.-]*\\.gguf$"));
  static const QRegularExpression hashPattern(QStringLiteral("^[0-9a-f]{64}$"));
  if (mDirectory.isEmpty() || !filePattern.match(mArtifact.name).hasMatch() || mArtifact.size <= 0
      || mArtifact.size > std::numeric_limits<qint64>::max() - 32 * 1024 * 1024
      || !hashPattern.match(QString::fromLatin1(mArtifact.sha256)).hasMatch()
      || !mArtifact.url.isValid() || mArtifact.url.host().isEmpty()
      || (mArtifact.url.scheme() != QLatin1String("https") && mArtifact.url.scheme() != QLatin1String("http")))
    return fail(error, QStringLiteral("Invalid model directory or locked artifact manifest"));
  return true;
}

bool ModelFiles::verifyPath(const QString &path, const std::atomic_bool *cancel, QString *error) const
{
  QFile file(path);
  if (cancelled(cancel)) return fail(error, QStringLiteral("Model operation cancelled"));
  if (!file.open(QIODevice::ReadOnly)) return fail(error, QStringLiteral("Cannot open model: %1").arg(file.errorString()));
  if (file.size() != mArtifact.size) return fail(error, QStringLiteral("Model size does not match the locked artifact"));
  QCryptographicHash hash(QCryptographicHash::Sha256);
  while (!file.atEnd())
  {
    if (cancelled(cancel)) return fail(error, QStringLiteral("Model operation cancelled"));
    const auto chunk = file.read(chunkSize);
    if (chunk.isEmpty() && file.error() != QFileDevice::NoError)
      return fail(error, QStringLiteral("Cannot read model: %1").arg(file.errorString()));
    hash.addData(chunk);
  }
  if (hash.result().toHex() != mArtifact.sha256) return fail(error, QStringLiteral("Model SHA-256 does not match the locked artifact"));
  return true;
}

bool ModelFiles::verify(const std::atomic_bool *cancel, QString *error) const
{
  if (error) error->clear();
  return valid(error) && verifyPath(filePath(), cancel, error);
}

bool ModelFiles::checkSpace(QString *error) const
{
  qint64 available = -1;
#ifdef Q_OS_MACOS
  // Qt 6.12 may map APFS firmlinks to the read-only System volume. Query the
  // actual destination volume as in QCopilots-Search's model downloader.
  struct statfs volume{};
  if (::statfs(QFile::encodeName(mDirectory).constData(), &volume) != 0 || (volume.f_flags & MNT_RDONLY) != 0)
    return fail(error, QStringLiteral("Model directory is unavailable or read-only"));
  const auto blockSize = quint64(volume.f_bsize);
  const auto blocks = quint64(volume.f_bavail);
  if (blockSize == 0 || volume.f_bsize == std::numeric_limits<decltype(volume.f_bsize)>::max()
      || blocks > quint64(std::numeric_limits<qint64>::max()) / blockSize)
    return fail(error, QStringLiteral("Cannot determine available model storage"));
  available = qint64(blocks * blockSize);
#else
  const QStorageInfo storage(mDirectory);
  if (!storage.isValid() || !storage.isReady() || storage.isReadOnly())
    return fail(error, QStringLiteral("Model directory is unavailable or read-only"));
  available = storage.bytesAvailable();
#endif
  if (available < mArtifact.size + 32 * 1024 * 1024)
    return fail(error, QStringLiteral("Model directory is unavailable or has insufficient free space"));
  return true;
}

bool ModelFiles::importFile(const QString &source, const std::atomic_bool *cancel, QString *error,
                           const Progress &progress) const
{
  if (error) error->clear();
  if (!valid(error) || !verifyPath(source, cancel, error)) return false;
  if (QFileInfo(source).canonicalFilePath() == QFileInfo(filePath()).canonicalFilePath()) return true;
  if (!QDir().mkpath(mDirectory)) return fail(error, QStringLiteral("Cannot create model directory"));
  QLockFile lock(lockPath(QDir(mDirectory).filePath(QStringLiteral("qctsm-models.lock"))));
  lock.setStaleLockTime(0);
  if (!lock.tryLock()) return fail(error, QStringLiteral("Another process is updating the model directory"));
  if (!checkSpace(error)) return false;
  QFile input(source);
  QSaveFile output(filePath());
  output.setDirectWriteFallback(false);
  if (!input.open(QIODevice::ReadOnly) || !output.open(QIODevice::WriteOnly))
    return fail(error, QStringLiteral("Cannot open model import source or destination"));
  QCryptographicHash hash(QCryptographicHash::Sha256);
  qint64 received = 0;
  while (!input.atEnd())
  {
    if (cancelled(cancel)) return fail(error, QStringLiteral("Model operation cancelled"));
    const auto chunk = input.read(chunkSize);
    if ((chunk.isEmpty() && input.error() != QFileDevice::NoError) || output.write(chunk) != chunk.size())
      return fail(error, QStringLiteral("Model import read or write failed"));
    received += chunk.size();
    if (received > mArtifact.size) return fail(error, QStringLiteral("Model import source changed while copying"));
    hash.addData(chunk);
    if (progress) progress(received, mArtifact.size);
  }
  if (cancelled(cancel)) return fail(error, QStringLiteral("Model operation cancelled"));
  if (received != mArtifact.size || hash.result().toHex() != mArtifact.sha256)
    return fail(error, QStringLiteral("Model import source changed while copying"));
  if (!output.commit()) return fail(error, QStringLiteral("Cannot publish model: %1").arg(output.errorString()));
  return true;
}

bool ModelFiles::downloadMissing(const std::atomic_bool *cancel, QString *error,
                                const Progress &progress) const
{
  if (error) error->clear();
  if (!valid(error)) return false;
  if (verify(cancel, nullptr)) return true;
  if (cancelled(cancel)) return fail(error, QStringLiteral("Model operation cancelled"));
  if (!QDir().mkpath(mDirectory)) return fail(error, QStringLiteral("Cannot create model directory"));
  QLockFile lock(lockPath(QDir(mDirectory).filePath(QStringLiteral("qctsm-models.lock"))));
  lock.setStaleLockTime(0);
  if (!lock.tryLock()) return fail(error, QStringLiteral("Another process is updating the model directory"));
  if (verify(cancel, nullptr)) return true;
  if (!checkSpace(error)) return false;
  QSaveFile output(filePath());
  output.setDirectWriteFallback(false);
  if (!output.open(QIODevice::WriteOnly)) return fail(error, QStringLiteral("Cannot create model download destination"));
  QNetworkAccessManager network;
  QNetworkRequest request(mArtifact.url);
  request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
  request.setTransferTimeout(60000);
  auto *reply = network.get(request);
  reply->setReadBufferSize(2 * chunkSize);
  QEventLoop loop;
  QTimer cancellationTimer;
  QTimer deadline;
  deadline.setSingleShot(true);
  QCryptographicHash hash(QCryptographicHash::Sha256);
  qint64 received = 0;
  QString failure;
  auto consume = [&] {
    const auto http = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (http != 200)
    {
      reply->readAll();
      return;
    }
    while (reply->bytesAvailable() > 0)
    {
      if (cancelled(cancel)) { failure = QStringLiteral("Model operation cancelled"); reply->abort(); return; }
      const auto chunk = reply->read(chunkSize);
      received += chunk.size();
      if (received > mArtifact.size) { failure = QStringLiteral("Download exceeds locked model size"); reply->abort(); return; }
      if (output.write(chunk) != chunk.size()) { failure = QStringLiteral("Cannot write downloaded model"); reply->abort(); return; }
      hash.addData(chunk);
      if (progress) progress(received, mArtifact.size);
    }
  };
  QObject::connect(reply, &QIODevice::readyRead, &loop, consume);
  QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
  QObject::connect(&cancellationTimer, &QTimer::timeout, &loop, [&] {
    if (cancelled(cancel)) { failure = QStringLiteral("Model operation cancelled"); reply->abort(); }
  });
  QObject::connect(&deadline, &QTimer::timeout, &loop, [&] {
    failure = QStringLiteral("Model download timed out"); reply->abort();
  });
  cancellationTimer.start(50);
  deadline.start(30 * 60 * 1000);
  if (!reply->isFinished()) loop.exec();
  consume();
  if (failure.isEmpty() && reply->error() != QNetworkReply::NoError) failure = reply->errorString();
  if (failure.isEmpty() && reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() != 200)
    failure = QStringLiteral("Model server returned HTTP %1").arg(reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt());
  if (failure.isEmpty() && (received != mArtifact.size || hash.result().toHex() != mArtifact.sha256))
    failure = QStringLiteral("Downloaded model size or SHA-256 does not match the locked artifact");
  delete reply;
  if (!failure.isEmpty()) return fail(error, failure);
  if (cancelled(cancel)) return fail(error, QStringLiteral("Model operation cancelled"));
  if (!output.commit()) return fail(error, QStringLiteral("Cannot publish downloaded model: %1").arg(output.errorString()));
  return true;
}
}
