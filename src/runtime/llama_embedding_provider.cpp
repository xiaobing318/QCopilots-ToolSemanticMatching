// SPDX-License-Identifier: MIT
// Text encoding and controlled backend loading adapted from QCopilots-Search
// and DigUp (MIT), Copyright (c) 2026 Abdur Rahim.
#include "llama_embedding_provider.h"
#include "model_files.h"
#include "BackendLoader.h"
#include "BackendPaths.h"
#include "BackendSelection.h"
#include "utf8_locale.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QThread>
#include <llama.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <limits>
#include <mutex>
#include <stdexcept>

#ifndef QCTSM_GPU_BACKEND
#define QCTSM_GPU_BACKEND "cpu"
#endif

namespace qctsm
{
namespace
{
QString packagedBackend() { return detail::canonicalBackendName(QString::fromLatin1(QCTSM_GPU_BACKEND)); }
void logCallback(ggml_log_level level, const char *text, void *)
{
  if (level >= GGML_LOG_LEVEL_WARN && text) std::fputs(text, stderr);
}
QStringList backendRoots()
{
#ifdef Q_OS_MACOS
  constexpr auto platform = detail::BackendPlatform::MacOS;
#elif defined(Q_OS_WIN)
  constexpr auto platform = detail::BackendPlatform::Windows;
#else
  constexpr auto platform = detail::BackendPlatform::Unix;
#endif
  return detail::cpuBackendSearchRoots(QCoreApplication::applicationDirPath(), platform);
}
QString libraryName(const QString &backend)
{
#ifdef Q_OS_WIN
  return QStringLiteral("ggml-%1.dll").arg(backend);
#else
  // The locked ggml build produces .so loadable backends on macOS as well.
  return QStringLiteral("libggml-%1.so").arg(backend);
#endif
}
bool registered(const QString &backend)
{
  for (size_t i = 0; i < ggml_backend_reg_count(); ++i)
    if (detail::canonicalBackendName(QString::fromUtf8(ggml_backend_reg_name(ggml_backend_reg_get(i)))) == backend)
      return true;
  return false;
}
bool loadModule(const QString &name)
{
  for (const auto &root : backendRoots())
  {
    const auto path = QDir(root).absoluteFilePath(libraryName(name));
    if (QFileInfo::exists(path) && ggml_backend_load(path.toUtf8().constData())) return true;
  }
  return false;
}
bool moduleExists(const QString &name)
{
  for (const auto &root : backendRoots())
    if (QFileInfo(QDir(root).absoluteFilePath(libraryName(name))).isFile()) return true;
  return false;
}
struct RuntimeDevices
{
  QVector<ggml_backend_dev_t> handles;
  QVector<detail::BackendDevice> descriptions;
  QString gpuFailureKind;
};
std::recursive_mutex &initializationMutex()
{
  static std::recursive_mutex mutex;
  return mutex;
}
RuntimeDevices initializeDevices(detail::BackendMode mode, QString *gpuError)
{
  std::lock_guard lock(initializationMutex());
  const ScopedUtf8Locale locale;
  if (!locale.isValid()) throw std::runtime_error("Cannot configure thread-local UTF-8 model paths");
  static std::once_flag initialized;
  std::call_once(initialized, [] {
    llama_log_set(logCallback, nullptr);
    if (!registered(QStringLiteral("cpu")))
    {
#ifdef QCTSM_CPU_DISPATCH
      const QStringList variants = qEnvironmentVariableIntValue("QCOPILOTS_TOOL_SEMANTIC_MATCHING_CPU_BASELINE") == 1
        ? QStringList{QStringLiteral("cpu-x64")}
        : QStringList{QStringLiteral("cpu-sapphirerapids"), QStringLiteral("cpu-zen4"),
                      QStringLiteral("cpu-cooperlake"), QStringLiteral("cpu-icelake"),
                      QStringLiteral("cpu-cascadelake"), QStringLiteral("cpu-cannonlake"),
                      QStringLiteral("cpu-skylakex"), QStringLiteral("cpu-alderlake"),
                      QStringLiteral("cpu-haswell"), QStringLiteral("cpu-piledriver"),
                      QStringLiteral("cpu-ivybridge"), QStringLiteral("cpu-sandybridge"),
                      QStringLiteral("cpu-sse42"), QStringLiteral("cpu-x64")};
#else
      const QStringList variants{QStringLiteral("cpu")};
#endif
      bool loaded = false;
      for (const auto &variant : variants)
        if (loadModule(variant)) { loaded = true; break; }
      if (!loaded) throw std::runtime_error("No compatible packaged CPU backend was found");
    }
    // A known CPU module is already registered, preventing llama's generic
    // initializer from searching the current directory or environment paths.
    llama_backend_init();
  });
  static std::once_flag gpuInitialized;
  static QString gpuFailure;
  static QString gpuFailureKind;
  if (mode != detail::BackendMode::Cpu)
    std::call_once(gpuInitialized, [] {
      const auto backend = packagedBackend();
      if (backend == QLatin1String("cpu") || registered(backend)) return;
      gpuFailure = detail::prepareGpuRuntime(QCoreApplication::applicationDirPath(), backend);
      if (!gpuFailure.isEmpty()) { gpuFailureKind = QStringLiteral("package"); return; }
      if (!moduleExists(backend))
      {
        gpuFailure = QStringLiteral("Packaged %1 GPU module is missing").arg(backend);
        gpuFailureKind = QStringLiteral("package");
        return;
      }
      if (!loadModule(backend) || !registered(backend))
      {
        gpuFailure = QStringLiteral("Cannot load packaged %1 GPU module or its runtime dependencies").arg(backend);
        // ggml does not distinguish absent drivers from a backend that fails
        // initialization. Never describe this result as confirmed no hardware.
        gpuFailureKind = QStringLiteral("initialization");
      }
    });
  *gpuError = mode == detail::BackendMode::Cpu ? QString{} : gpuFailure;
  RuntimeDevices result;
  result.gpuFailureKind = mode == detail::BackendMode::Cpu ? QString{} : gpuFailureKind;
  for (size_t i = 0; i < ggml_backend_dev_count(); ++i)
  {
    auto device = ggml_backend_dev_get(i);
    ggml_backend_dev_props properties{};
    ggml_backend_dev_get_props(device, &properties);
    const auto type = properties.type;
    result.handles.append(device);
    result.descriptions.append({detail::canonicalBackendName(QString::fromUtf8(ggml_backend_reg_name(ggml_backend_dev_backend_reg(device)))),
      QString::fromUtf8(properties.name ? properties.name : ""), QString::fromUtf8(properties.description ? properties.description : ""),
      type == GGML_BACKEND_DEVICE_TYPE_CPU,
      type == GGML_BACKEND_DEVICE_TYPE_GPU || type == GGML_BACKEND_DEVICE_TYPE_IGPU,
      {}, QString::fromUtf8(properties.device_id ? properties.device_id : ""),
      quint64(properties.memory_total), quint64(properties.memory_free)});
  }
  // Metal can expose an unnamed wrapper, or a device without the required
  // compute capability. This is a capability query only. Actual GPU execution
  // is verified by verifyGpu before a model is loaded on a selected device.
  for (qsizetype index = 0; index < result.descriptions.size(); ++index)
  {
    auto &device = result.descriptions[index];
    if (!device.gpu) continue;
    device.unavailableReason = detail::deviceIdentityError(device, result.descriptions);
    if (device.backend != QLatin1String("metal") || !device.unavailableReason.isEmpty()) continue;
    if (device.name.isEmpty())
    {
      device.unavailableReason = QStringLiteral("Metal did not discover a physical GPU device");
      continue;
    }
    std::unique_ptr<ggml_context, decltype(&ggml_free)> metadata(
      ggml_init({3 * ggml_tensor_overhead(), nullptr, true}), &ggml_free);
    if (!metadata) throw std::runtime_error("Cannot allocate GPU capability query metadata");
    auto *a = ggml_new_tensor_2d(metadata.get(), GGML_TYPE_F32, 32, 32);
    auto *b = ggml_new_tensor_2d(metadata.get(), GGML_TYPE_F32, 32, 32);
    auto *product = ggml_mul_mat(metadata.get(), a, b);
    if (!ggml_backend_dev_supports_op(result.handles.at(index), product))
      device.unavailableReason = QStringLiteral("Metal device does not support required 32x32 F32 matrix multiplication");
  }
  return result;
}
QJsonObject deviceJson(const detail::BackendDevice &device)
{
  return {{"backend", device.backend}, {"name", device.name}, {"device_id", device.deviceId},
          {"description", device.description}, {"gpu", device.gpu},
          {"available", device.cpu || (device.gpu && device.unavailableReason.isEmpty())},
          {"unavailable_reason", device.unavailableReason},
          {"memory_total", qint64(device.memoryTotal)}, {"memory_free", qint64(device.memoryFree)}};
}
void verifyGpu(ggml_backend_dev_t device)
{
  constexpr size_t side = 32;
  std::unique_ptr<ggml_backend, decltype(&ggml_backend_free)> backend(ggml_backend_dev_init(device, nullptr), &ggml_backend_free);
  if (!backend || ggml_backend_get_device(backend.get()) != device)
    throw std::runtime_error("Cannot initialize selected GPU for compute self-test");
  std::unique_ptr<ggml_context, decltype(&ggml_free)> context(
    ggml_init({3 * ggml_tensor_overhead() + ggml_graph_overhead_custom(8, false), nullptr, true}), &ggml_free);
  if (!context) throw std::runtime_error("Cannot allocate GPU self-test metadata");
  auto *a = ggml_new_tensor_2d(context.get(), GGML_TYPE_F32, side, side);
  auto *b = ggml_new_tensor_2d(context.get(), GGML_TYPE_F32, side, side);
  auto *product = ggml_mul_mat(context.get(), a, b);
  auto *graph = ggml_new_graph_custom(context.get(), 8, false);
  ggml_build_forward_expand(graph, product);
  if (!ggml_backend_supports_op(backend.get(), product)) throw std::runtime_error("Selected GPU cannot execute required matrix multiplication");
  std::unique_ptr<ggml_backend_buffer, decltype(&ggml_backend_buffer_free)> buffer(
    ggml_backend_alloc_ctx_tensors(context.get(), backend.get()), &ggml_backend_buffer_free);
  if (!buffer) throw std::runtime_error("Cannot allocate GPU self-test tensors");
  std::array<float, side * side> av{}, bv{}, expected{}, actual{};
  for (size_t i = 0; i < av.size(); ++i) { av[i] = float(i % 7 + 1); bv[i] = float(i % 13 + 2); }
  for (size_t row = 0; row < side; ++row)
    for (size_t col = 0; col < side; ++col)
      for (size_t k = 0; k < side; ++k) expected[row * side + col] += av[col * side + k] * bv[row * side + k];
  ggml_backend_tensor_set(a, av.data(), 0, sizeof(av));
  ggml_backend_tensor_set(b, bv.data(), 0, sizeof(bv));
  if (ggml_backend_graph_compute(backend.get(), graph) != GGML_STATUS_SUCCESS)
    throw std::runtime_error("Selected GPU compute self-test failed");
  ggml_backend_synchronize(backend.get());
  ggml_backend_tensor_get(product, actual.data(), 0, sizeof(actual));
  for (size_t i = 0; i < actual.size(); ++i)
    if (!std::isfinite(actual[i]) || std::abs(actual[i] - expected[i]) > .001f)
      throw std::runtime_error("Selected GPU returned an incorrect self-test result");
}
}

class LlamaEmbeddingProvider::Impl
{
  public:
    explicit Impl(ModelOptions value) : options(std::move(value)) {}
    ~Impl() { close(); }
    ModelOptions options;
    mutable std::timed_mutex mutex;
    const std::atomic_bool *cancel = nullptr;
    llama_model *model = nullptr;
    llama_context *context = nullptr;
    llama_batch_ext *batch = nullptr;
    const llama_vocab *vocab = nullptr;
    detail::BackendSelection selection;
    detail::BackendDevice device;
    bool gpuVerified = false;
    static bool abort(void *value)
    {
      const auto *self = static_cast<Impl *>(value);
      return self->cancel && self->cancel->load(std::memory_order_relaxed);
    }
    static bool progress(float, void *value) { return !abort(value); }
    void checkCancel() const { if (abort(const_cast<Impl *>(this))) throw std::runtime_error("Embedding cancelled"); }
    void close()
    {
      if (batch) llama_batch_ext_free(batch);
      if (context) llama_free(context);
      if (model) llama_model_free(model);
      batch = nullptr; context = nullptr; model = nullptr; vocab = nullptr; gpuVerified = false;
    }
    void loadOnDevice(ggml_backend_dev_t handle)
    {
      checkCancel();
      if (selection.gpu) { verifyGpu(handle); gpuVerified = true; }
      checkCancel();
      const ScopedUtf8Locale locale;
      if (!locale.isValid()) throw std::runtime_error("Cannot configure thread-local UTF-8 model path");
      auto modelParams = llama_model_default_params();
      ggml_backend_dev_t selected[] = {selection.gpu ? handle : nullptr, nullptr};
      modelParams.devices = selected;
      modelParams.n_gpu_layers = selection.gpu ? -1 : 0;
      modelParams.split_mode = LLAMA_SPLIT_MODE_NONE;
      modelParams.progress_callback = &Impl::progress;
      modelParams.progress_callback_user_data = this;
      const auto path = ModelFiles(options.modelDirectory).filePath().toUtf8();
      model = llama_model_load_from_file(path.constData(), modelParams);
      checkCancel();
      if (!model) throw std::runtime_error("Cannot load the locked EmbeddingGemma 2 model");
      if (llama_model_n_embd_out(model) != 768) throw std::runtime_error("Embedding model output dimension must be 768");
      auto params = llama_context_default_params();
      params.n_ctx = uint32_t(options.contextTokens);
      params.n_batch = uint32_t(options.contextTokens);
      params.n_ubatch = uint32_t(options.contextTokens);
      params.n_seq_max = 1;
      params.kv_unified = true;
      params.embeddings = true;
      params.pooling_type = LLAMA_POOLING_TYPE_MEAN;
      const int available = std::max(1, QThread::idealThreadCount());
      params.n_threads = std::clamp(options.threads > 0 ? options.threads : std::max(1, available / 2), 1, available);
      params.n_threads_batch = params.n_threads;
      params.abort_callback = &Impl::abort;
      params.abort_callback_data = this;
      params.offload_kqv = selection.gpu;
      params.op_offload = selection.gpu;
      context = llama_init_from_model(model, params);
      if (!context) throw std::runtime_error("Cannot create embedding model context");
      vocab = llama_model_get_vocab(model);
      batch = llama_batch_ext_init(context);
      if (!batch) throw std::runtime_error("Cannot allocate embedding batch");
      encode(QStringLiteral("task: search result | query: warm up"));
    }
    void ensureLoaded()
    {
      if (context) return;
      if (options.contextTokens < 16 || options.contextTokens > 2048)
        throw std::runtime_error("Embedding context must be between 16 and 2048 tokens");
      QString error;
      if (!ModelFiles(options.modelDirectory).verify(cancel, &error)) throw std::runtime_error(error.toStdString());
      const auto mode = detail::parseBackendMode(options.backend, &error);
      if (!mode) throw std::runtime_error(error.toStdString());
      const auto devices = initializeDevices(*mode, &error);
      const bool explicitDevice = !options.deviceId.isEmpty() || !options.deviceName.isEmpty();
      selection = explicitDevice
        ? detail::selectBackendDevice(*mode, packagedBackend(), devices.descriptions,
            {*mode == detail::BackendMode::Cpu ? QStringLiteral("cpu") : packagedBackend(),
             options.deviceId, options.deviceName}, error)
        : detail::selectBackend(*mode, packagedBackend(), devices.descriptions, error);
      if (!selection.error.isEmpty()) throw std::runtime_error(selection.error.toStdString());
      device = devices.descriptions.at(selection.deviceIndex);
      try { loadOnDevice(devices.handles.at(selection.deviceIndex)); }
      catch (const std::exception &failure)
      {
        if (explicitDevice || *mode != detail::BackendMode::Auto || !selection.gpu || abort(this)) throw;
        const auto reason = QString::fromUtf8(failure.what());
        close();
        selection = detail::selectBackend(detail::BackendMode::Cpu, packagedBackend(), devices.descriptions);
        if (!selection.error.isEmpty()) throw std::runtime_error(selection.error.toStdString());
        selection.fallbackReason = reason;
        device = devices.descriptions.at(selection.deviceIndex);
        loadOnDevice(devices.handles.at(selection.deviceIndex));
      }
    }
    QVector<float> encode(const QString &text)
    {
      checkCancel();
      const auto utf8 = text.toUtf8();
      if (utf8.isEmpty()) throw std::runtime_error("Embedding input text must not be empty");
      if (utf8.size() > 4 * 1024 * 1024) throw std::runtime_error("Embedding input exceeds the 4 MiB text limit");
      QVector<llama_token> tokens(utf8.size() + 8);
      auto count = llama_tokenize(vocab, utf8.constData(), int32_t(utf8.size()), tokens.data(), int32_t(tokens.size()), true, true);
      if (count < 0)
      {
        if (count == std::numeric_limits<int32_t>::min()) throw std::runtime_error("Model tokenizer failed");
        tokens.resize(-count);
        count = llama_tokenize(vocab, utf8.constData(), int32_t(utf8.size()), tokens.data(), int32_t(tokens.size()), true, true);
      }
      if (count <= 0) throw std::runtime_error("Model tokenizer returned no tokens");
      if (count > options.contextTokens)
        throw std::runtime_error(QStringLiteral("Embedding input has %1 tokens, exceeding the %2 token limit").arg(count).arg(options.contextTokens).toStdString());
      llama_batch_ext_clear(batch);
      for (int i = 0; i < count; ++i)
      {
        const auto index = llama_batch_ext_add_token(batch, 0, tokens.at(i));
        const llama_pos pos[] = {i, 0, 0, 0};
        if (index < 0 || !llama_batch_ext_set_pos(batch, index, pos)
            || !llama_batch_ext_set_output_embd(batch, index, true))
          throw std::runtime_error("Cannot construct embedding input batch");
      }
      const auto status = llama_process(context, LLAMA_PROCESS_TYPE_ENCODE, batch);
      checkCancel();
      if (status != 0) throw std::runtime_error("llama_process failed to encode the text");
      const float *pooled = llama_get_embeddings_seq(context, 0);
      if (!pooled) throw std::runtime_error("Model returned no pooled embedding");
      double square = 0;
      for (int i = 0; i < 768; ++i)
      {
        if (!std::isfinite(pooled[i])) throw std::runtime_error("Model returned a non-finite embedding");
        square += double(pooled[i]) * pooled[i];
      }
      if (!(square > 0)) throw std::runtime_error("Model returned a zero embedding");
      QVector<float> result(768);
      const double scale = 1.0 / std::sqrt(square);
      for (int i = 0; i < 768; ++i) result[i] = float(pooled[i] * scale);
      return result;
    }
};

LlamaEmbeddingProvider::LlamaEmbeddingProvider(ModelOptions options) : d(std::make_unique<Impl>(std::move(options))) {}
LlamaEmbeddingProvider::~LlamaEmbeddingProvider() = default;

QList<QVector<float>> LlamaEmbeddingProvider::embed(const QStringList &texts, const std::atomic_bool *cancel, QString *error)
{
  if (error) error->clear();
  std::unique_lock<std::timed_mutex> lock(d->mutex, std::defer_lock);
  while (!lock.try_lock_for(std::chrono::milliseconds(50)))
    if (cancel && cancel->load(std::memory_order_relaxed))
    { if (error) *error = QStringLiteral("Embedding cancelled"); return {}; }
  d->cancel = cancel;
  struct ResetCancel { Impl *value; ~ResetCancel() { value->cancel = nullptr; } } reset{d.get()};
  try
  {
    d->checkCancel();
    if (texts.isEmpty() || texts.size() > 128) throw std::runtime_error("Embedding batch must contain between 1 and 128 texts");
    for (const auto &text : texts)
      if (text.isEmpty()) throw std::runtime_error("Embedding input text must not be empty");
    d->ensureLoaded();
    QList<QVector<float>> results;
    results.reserve(texts.size());
    for (const auto &text : texts) results.append(d->encode(text));
    return results;
  }
  catch (const std::exception &failure)
  {
    // Discard partial results, and recreate a potentially interrupted context.
    d->close();
    if (error) *error = QString::fromUtf8(failure.what());
    return {};
  }
}

QJsonObject LlamaEmbeddingProvider::modelInfo() const
{
  std::lock_guard lock(d->mutex);
  return {{"model", QStringLiteral("embeddinggemma-2-Q8_0")}, {"dimensions", 768},
          {"revision", EmbeddingProfile{}.revision}, {"loaded", d->context != nullptr},
          {"backend", d->selection.backend.isEmpty() ? d->options.backend : d->selection.backend},
          {"packaged_backend", packagedBackend()}, {"device", deviceJson(d->device)},
          {"gpu_compute_verified", d->gpuVerified}, {"fallback_reason", d->selection.fallbackReason},
          {"context_tokens", d->options.contextTokens}, {"pooling", QStringLiteral("mean")},
          {"normalization", QStringLiteral("l2")}};
}

QJsonObject LlamaEmbeddingProvider::devices(const QString &backend)
{
  try
  {
    QString error;
    const auto mode = detail::parseBackendMode(backend, &error);
    if (!mode) return {{"status", "failed"}, {"message", error}};
    const auto devices = initializeDevices(*mode, &error);
    QJsonArray entries;
    for (const auto &device : devices.descriptions) entries.append(deviceJson(device));
    return {{"status", "succeeded"}, {"packaged_backend", packagedBackend()}, {"devices", entries},
            {"gpu_load_error", error}, {"gpu_load_error_kind", devices.gpuFailureKind}};
  }
  catch (const std::exception &failure)
  { return {{"status", "failed"}, {"message", QString::fromUtf8(failure.what())}}; }
}
}
