// SPDX-License-Identifier: MIT
#pragma once
#include "service_config.h"
#include "core/retrieval.h"
#include <QJsonObject>
#include <memory>

namespace qctsm {
class TaskService final {
public:
  explicit TaskService(const ServiceConfig &config);
  ~TaskService();
  QJsonObject submit(const QString &operation, const QJsonObject &payload);
  QJsonObject snapshot(const QString &id) const;
  QJsonObject cancel(const QString &id);
  QJsonObject getRecords(const QStringList &ids);
  QJsonObject indexStatus();
  QJsonObject modelInfo() const;
  QJsonObject embeddings(const QJsonObject &body, const std::atomic_bool *cancel = nullptr);
  void stop();
private:
  class Impl;
  std::unique_ptr<Impl> d;
};
QJsonObject normalizeCatalog(const QJsonValue &value, const QString &sourceId);
}
