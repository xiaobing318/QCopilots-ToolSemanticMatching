// SPDX-License-Identifier: MIT
#include "service_config.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSaveFile>
#include <QStandardPaths>
#include <QUuid>

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
  if(c.provider!=QStringLiteral("embedded") && c.provider!=QStringLiteral("http")) { if(error) *error=QStringLiteral("provider must be embedded or http"); }
  else if(c.host!=QStringLiteral("127.0.0.1") && c.host!=QStringLiteral("::1") && c.host!=QStringLiteral("localhost")) { if(error) *error=QStringLiteral("Service host must be a loopback address"); }
  else if(c.port<0 || c.port>65535 || c.threads<1 || c.threads>256 || c.timeoutMs<100 || c.timeoutMs>3600000) { if(error) *error=QStringLiteral("Invalid port, threads or timeout_ms"); }
  if(QDir::isRelativePath(c.databasePath)) c.databasePath=QDir(c.profilePath).absoluteFilePath(c.databasePath);
  if(QDir::isRelativePath(c.modelPath)) c.modelPath=QDir(c.profilePath).absoluteFilePath(c.modelPath);
  return c;
}
QJsonObject ServiceConfig::toJson(bool secrets) const {
  return {{"schema_version",1},{"database_path",databasePath},{"provider",provider},{"model_path",modelPath},{"model_id",modelId},{"model_revision",modelRevision},{"api_base",apiBase},{"authorization",secrets?authorization:QString()},{"backend",backend},{"device_id",deviceId},{"threads",threads},{"host",host},{"port",port},{"token",secrets?token:QString()},{"timeout_ms",timeoutMs}};
}
bool ServiceConfig::save(QString *error) const {
  if(error) error->clear();
  if(!QDir().mkpath(profilePath)) { if(error) *error=QStringLiteral("Cannot create profile directory"); return false; }
  QSaveFile file(configFilePath());
  if(!file.open(QIODevice::WriteOnly)) { if(error) *error=file.errorString(); return false; }
  file.setPermissions(QFileDevice::ReadOwner|QFileDevice::WriteOwner);
  const auto bytes=QJsonDocument(toJson()).toJson(QJsonDocument::Indented);
  if(file.write(bytes)!=bytes.size() || !file.commit()) { if(error) *error=file.errorString(); return false; }
  return true;
}
}
