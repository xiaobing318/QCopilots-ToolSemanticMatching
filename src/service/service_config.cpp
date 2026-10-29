// SPDX-License-Identifier: MIT
#include "service_config.h"
#include "runtime/model_files.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonArray>
#include <QSaveFile>
#include <QStandardPaths>
#include <QUuid>
#include <QUrl>

namespace qctsm {
QString ServiceConfig::defaultProfilePath() {
  return QDir(QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)).filePath(QStringLiteral("QCopilots/ToolSemanticMatching"));
}
QString ServiceConfig::configFilePath() const { return QDir(profilePath).filePath(QStringLiteral("config.json")); }
ServiceConfig ServiceConfig::load(const QString &path, QString *error) {
  if (error) error->clear();
  ServiceConfig c;
  c.profilePath = QDir(path.isEmpty() ? defaultProfilePath() : path).absolutePath();
  c.databasePath = QDir(c.profilePath).filePath(QStringLiteral("index.sqlite"));
  c.modelPath = QDir(c.profilePath).filePath(QStringLiteral("models/embeddinggemma-2-Q8_0.gguf"));
  QFile f(c.configFilePath());
  if (!f.exists()) { c.token = QUuid::createUuid().toString(QUuid::WithoutBraces); return c; }
  if (!f.open(QIODevice::ReadOnly)) { if(error) *error=f.errorString(); return c; }
  if(f.size()>1024*1024) { if(error) *error=QStringLiteral("Configuration exceeds 1 MiB"); return c; }
  QJsonParseError parse;
  const auto doc=QJsonDocument::fromJson(f.readAll(), &parse);
  if(!doc.isObject() || parse.error!=QJsonParseError::NoError) { if(error) *error=QStringLiteral("Invalid configuration JSON: ")+parse.errorString(); return c; }
  const auto o=doc.object();
  if(o.value(QStringLiteral("schema_version")).toInt(1)!=1) { if(error) *error=QStringLiteral("Unsupported configuration schema"); return c; }
  auto str=[&](const char *key, const QString &fallback) { return o.value(QLatin1String(key)).toString(fallback); };
  c.databasePath=str("database_path", c.databasePath); c.provider=str("provider", c.provider);
  c.modelPath=str("model_path",c.modelPath); c.modelId=str("model_id",c.modelId); c.modelRevision=str("model_revision",c.modelRevision);
  c.apiBase=str("api_base",c.apiBase); c.authorization=str("authorization",{}); c.backend=str("backend",c.backend); c.deviceId=str("device_id",{});
  c.host=str("host",c.host); c.port=o.value(QStringLiteral("port")).toInt(8383); c.threads=o.value(QStringLiteral("threads")).toInt(4);
  c.token=str("token",{}); c.timeoutMs=o.value(QStringLiteral("timeout_ms")).toInt(120000);
  if(QDir::isRelativePath(c.databasePath)) c.databasePath=QDir(c.profilePath).absoluteFilePath(c.databasePath);
  if(QDir::isRelativePath(c.modelPath)) c.modelPath=QDir(c.profilePath).absoluteFilePath(c.modelPath);
  c.validate(error);
  return c;
}
QJsonObject ServiceConfig::toJson(bool secrets) const {
  return {{"schema_version",1},{"database_path",databasePath},{"provider",provider},{"model_path",modelPath},{"model_id",modelId},{"model_revision",modelRevision},{"api_base",apiBase},{"authorization",secrets?authorization:QString()},{"backend",backend},{"device_id",deviceId},{"threads",threads},{"host",host},{"port",port},{"token",secrets?token:QString()},{"timeout_ms",timeoutMs}};
}
bool ServiceConfig::validate(QString *error) const {
  if(error) error->clear();
  QString problem;
  if(provider!=QStringLiteral("embedded")&&provider!=QStringLiteral("http"))problem=QStringLiteral("provider must be embedded or http");
  else if(host!=QStringLiteral("127.0.0.1")&&host!=QStringLiteral("::1")&&host!=QStringLiteral("localhost"))problem=QStringLiteral("Service host must be a loopback address");
  else if(port<0||port>65535||threads<1||threads>256||timeoutMs<100||timeoutMs>3600000)problem=QStringLiteral("Invalid port, threads or timeout_ms");
  else if(databasePath.isEmpty()||modelId.isEmpty()||modelRevision.isEmpty())problem=QStringLiteral("Database path, model ID and revision are required");
  else if(provider==QStringLiteral("embedded")) {
    const auto locked=ModelFiles::manifest().value(QStringLiteral("models")).toArray().first().toObject();
    if(modelId!=locked.value(QStringLiteral("id")).toString()||modelRevision!=locked.value(QStringLiteral("revision")).toString())problem=QStringLiteral("Embedded model ID and revision must match the locked model manifest");
    else if(backend!=QStringLiteral("cpu")&&backend!=QStringLiteral("auto")&&backend!=QStringLiteral("gpu"))problem=QStringLiteral("backend must be cpu, auto or gpu");
    else if(modelPath.isEmpty())problem=QStringLiteral("Embedded model path is required");
  } else {
    const QUrl url(apiBase);
    if(!url.isValid()||(url.scheme()!=QStringLiteral("http")&&url.scheme()!=QStringLiteral("https"))||url.host().isEmpty()||!url.userInfo().isEmpty()||url.hasQuery()||url.hasFragment())problem=QStringLiteral("api_base must be an HTTP(S) URL without user information, query or fragment");
  }
  if(error)*error=problem;
  return problem.isEmpty();
}
bool ServiceConfig::save(QString *error) const {
  if(!validate(error))return false;
  if(!QDir().mkpath(profilePath)) { if(error) *error=QStringLiteral("Cannot create profile directory"); return false; }
  QSaveFile file(configFilePath());
  if(!file.open(QIODevice::WriteOnly)) { if(error) *error=file.errorString(); return false; }
  file.setPermissions(QFileDevice::ReadOwner|QFileDevice::WriteOwner);
  const auto bytes=QJsonDocument(toJson()).toJson(QJsonDocument::Indented);
  if(file.write(bytes)!=bytes.size() || !file.commit()) { if(error) *error=file.errorString(); return false; }
  return true;
}
}
