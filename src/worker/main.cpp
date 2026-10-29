// SPDX-License-Identifier: MIT
#include "runtime/llama_embedding_provider.h"
#include "runtime/model_files.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <atomic>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace
{
std::mutex outputMutex;
void write(QJsonObject response)
{
  const auto json = QJsonDocument(response).toJson(QJsonDocument::Compact);
  std::lock_guard lock(outputMutex);
  std::cout.write(json.constData(), json.size());
  std::cout << '\n' << std::flush;
}
void fail(const QString &id, const QString &code, const QString &message)
{
  write({{"id", id}, {"status", "failed"}, {"error_code", code}, {"message", message}});
}
// Drain oversized requests without allocating unbounded stdin buffers.
bool readLine(QByteArray &line, bool &oversized)
{
  line.clear(); oversized = false;
  char c;
  bool received = false;
  while (std::cin.get(c))
  {
    received = true;
    if (c == '\n') break;
    if (line.size() < 16 * 1024 * 1024) line.append(c);
    else oversized = true;
  }
  return received;
}
}

int main(int argc, char **argv)
{
  std::ios::sync_with_stdio(false);
  std::cin.tie(nullptr);
  QCoreApplication app(argc, argv);
  app.setApplicationName(QStringLiteral("qctsm-worker"));
  app.setApplicationVersion(QStringLiteral(QCTSM_VERSION));
  QCommandLineParser parser;
  parser.setApplicationDescription(QStringLiteral("QCopilots isolated text embedding worker (JSON lines)"));
  parser.addHelpOption();
  parser.addVersionOption();
  parser.addOption({"model-dir", "Directory containing the locked GGUF", "directory"});
  parser.addOption({"model", "Path to the locked embeddinggemma-2-Q8_0.gguf", "file"});
  parser.addOption({"backend", "Inference policy: cpu, auto, gpu", "policy", "cpu"});
  parser.addOption({"device-id", "Explicit backend hardware device ID", "id"});
  parser.addOption({"device-name", "Explicit unique device name (only when no hardware ID exists)", "name"});
  parser.addOption({"threads", "CPU worker threads, zero selects half the available CPUs", "count", "0"});
  parser.addOption({"context-tokens", "Per-text context limit (16 through 2048)", "count", "2048"});
  parser.process(app);
  qctsm::ModelOptions options;
  options.modelDirectory = parser.value("model-dir");
  if (parser.isSet("model"))
  {
    const QFileInfo model(parser.value("model"));
    if (model.fileName() != QLatin1String("embeddinggemma-2-Q8_0.gguf"))
    { std::cerr << "--model must reference embeddinggemma-2-Q8_0.gguf\n"; return 2; }
    if (!options.modelDirectory.isEmpty() && QFileInfo(options.modelDirectory).absoluteFilePath() != model.absolutePath())
    { std::cerr << "--model and --model-dir disagree\n"; return 2; }
    options.modelDirectory = model.absolutePath();
  }
  options.backend = parser.value("backend");
  options.deviceId = parser.value("device-id");
  options.deviceName = parser.value("device-name");
  bool validThreads = false, validContext = false;
  options.threads = parser.value("threads").toInt(&validThreads);
  options.contextTokens = parser.value("context-tokens").toInt(&validContext);
  if (!validThreads || options.threads < 0 || !validContext || options.contextTokens < 16 || options.contextTokens > 2048
      || !QStringList{"cpu", "auto", "gpu"}.contains(options.backend))
  { std::cerr << "Invalid thread count, context limit or backend policy\n"; return 2; }
  qctsm::LlamaEmbeddingProvider provider(options);
  std::atomic_bool busy{false};
  std::mutex activeMutex;
  QString activeId;
  std::shared_ptr<std::atomic_bool> cancellation;
  std::thread worker;
  auto cancelActive = [&] {
    std::lock_guard lock(activeMutex);
    if (cancellation) cancellation->store(true, std::memory_order_relaxed);
  };
  QByteArray line;
  bool oversized = false;
  while (readLine(line, oversized))
  {
    if (oversized) { fail({}, "request_too_large", "Worker request exceeds 16 MiB"); continue; }
    if (line.trimmed().isEmpty()) continue;
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(line, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject())
    { fail({}, "invalid_json", "Worker request must be a JSON object"); continue; }
    const auto request = document.object();
    const auto id = request.value("id").toString();
    if (id.isEmpty() || id.size() > 256)
    { fail({}, "invalid_id", "Request id must be a nonempty string of at most 256 characters"); continue; }
    const auto operation = request.value("operation").toString();
    if (operation == QLatin1String("quit") || operation == QLatin1String("shutdown"))
    {
      cancelActive();
      if (worker.joinable()) worker.join();
      write({{"id", id}, {"status", "succeeded"}});
      return 0;
    }
    if (operation == QLatin1String("cancel"))
    {
      const auto target = request.value("request_id").toString();
      std::lock_guard lock(activeMutex);
      if (target.isEmpty() || !busy.load() || target != activeId || !cancellation)
        fail(id, "request_not_active", "Cancellation target is not the active request");
      else
      {
        cancellation->store(true, std::memory_order_relaxed);
        write({{"id", id}, {"status", "succeeded"}, {"request_id", target}, {"state", "cancelling"}});
      }
      continue;
    }
    if (busy.load(std::memory_order_acquire))
    { fail(id, "busy", "The worker is processing another request"); continue; }
    if (worker.joinable()) worker.join();
    if (operation == QLatin1String("model_info"))
    {
      write({{"id", id}, {"status", "succeeded"}, {"runtime", provider.modelInfo()}, {"manifest", qctsm::ModelFiles::manifest()}});
      continue;
    }
    if (operation == QLatin1String("devices"))
    {
      auto result = qctsm::LlamaEmbeddingProvider::devices(options.backend);
      result.insert("id", id);
      write(result);
      continue;
    }
    if (operation != QLatin1String("embed"))
    { fail(id, "unknown_operation", "Expected embed, model_info, devices, cancel or quit"); continue; }
    if (request.contains("model") && request.value("model").toString() != QLatin1String("embeddinggemma-2-Q8_0"))
    { fail(id, "unknown_model", "Only embeddinggemma-2-Q8_0 is supported"); continue; }
    const auto input = request.value("input");
    QStringList texts;
    bool valid = true;
    if (input.isString()) texts.append(input.toString());
    else if (input.isArray())
      for (const auto &item : input.toArray())
      {
        if (!item.isString()) { valid = false; break; }
        texts.append(item.toString());
      }
    else valid = false;
    if (!valid || texts.isEmpty() || texts.size() > 128)
    { fail(id, "invalid_input", "input must contain between 1 and 128 text strings"); continue; }
    auto token = std::make_shared<std::atomic_bool>(false);
    {
      std::lock_guard lock(activeMutex);
      activeId = id;
      cancellation = token;
      busy.store(true, std::memory_order_release);
    }
    worker = std::thread([&, id, texts, token] {
      QString error;
      const auto vectors = provider.embed(texts, token.get(), &error);
      QJsonObject result{{"id", id}};
      if (token->load(std::memory_order_relaxed))
      {
        result.insert("status", "cancelled"); result.insert("error_code", "cancelled");
        result.insert("message", "Embedding request cancelled");
      }
      else if (vectors.size() != texts.size() || !error.isEmpty())
      {
        result.insert("status", "failed"); result.insert("error_code", "embedding_failed");
        result.insert("message", error.isEmpty() ? QStringLiteral("Incomplete embedding batch") : error);
      }
      else
      {
        QJsonArray data;
        for (const auto &vector : vectors)
        {
          QJsonArray values;
          for (const auto value : vector) values.append(double(value));
          data.append(values);
        }
        result.insert("status", "succeeded"); result.insert("data", data);
        result.insert("model", "embeddinggemma-2-Q8_0"); result.insert("dimensions", 768);
        result.insert("runtime", provider.modelInfo());
      }
      busy.store(false, std::memory_order_release);
      write(result);
    });
  }
  cancelActive();
  if (worker.joinable()) worker.join();
  return 0;
}
