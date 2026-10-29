// SPDX-License-Identifier: MIT
#pragma once
#include <QJsonObject>
#include <QString>

namespace qctsm {
struct ServiceConfig {
  QString profilePath;
  QString databasePath;
  QString provider = QStringLiteral("embedded");
  QString modelPath;
  QString modelId = QStringLiteral("embeddinggemma-2-Q8_0");
  QString modelRevision = QStringLiteral("bfcd298762cc34d0357ece5ebdd31791a3a374d8");
  QString apiBase = QStringLiteral("http://127.0.0.1:8282/v1/");
  QString authorization;
  QString backend = QStringLiteral("cpu");
  QString deviceId;
  int threads = 4;
  QString host = QStringLiteral("127.0.0.1");
  int port = 8383;
  QString token;
  int timeoutMs = 120000;
  static QString defaultProfilePath();
  static ServiceConfig load(const QString &path = {}, QString *error = nullptr);
  QString configFilePath() const;
  QJsonObject toJson(bool includeSecrets = true) const;
  bool validate(QString *error = nullptr) const;
  bool save(QString *error = nullptr) const;
};
}
