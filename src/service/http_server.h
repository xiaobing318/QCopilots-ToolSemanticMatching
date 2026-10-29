// SPDX-License-Identifier: MIT
#pragma once
#include "service_config.h"
#include <memory>

namespace qctsm {
class TaskService;
class HttpServer final {
public:
  HttpServer(const ServiceConfig &config,std::shared_ptr<TaskService> service);
  ~HttpServer();
  bool start(QString *error = nullptr);
  void stop();
  int port() const;
  bool shutdownRequested() const;
private:
  class Impl;
  std::unique_ptr<Impl> d;
};
}
