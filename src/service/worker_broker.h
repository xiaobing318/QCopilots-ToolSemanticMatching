// SPDX-License-Identifier: MIT
#pragma once
#include "core/retrieval.h"
#include "service_config.h"
#include <memory>

namespace qctsm {
// A single owned worker processes one text at a time, allowing an interactive
// request to take precedence at the next inference boundary during indexing.
class WorkerBroker final {
public:
  explicit WorkerBroker(const ServiceConfig &config);
  ~WorkerBroker();
  QList<QVector<float>> embed(const QStringList &texts, int priority,
                             const std::atomic_bool *cancel, QString *error);
  void stop();
private:
  class Impl;
  std::unique_ptr<Impl> d;
};
class WorkerEmbeddingProvider final : public EmbeddingProvider {
public:
  WorkerEmbeddingProvider(std::shared_ptr<WorkerBroker> broker, int priority):mBroker(std::move(broker)),mPriority(priority){}
  QList<QVector<float>> embed(const QStringList &texts,const std::atomic_bool *cancel,QString *error) override {return mBroker->embed(texts,mPriority,cancel,error);}
private:
  std::shared_ptr<WorkerBroker> mBroker;
  int mPriority;
};
}
