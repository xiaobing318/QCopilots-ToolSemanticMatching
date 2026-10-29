// SPDX-License-Identifier: MIT
// Short text golden inputs/vectors adapted from DigUp and QCopilots-Search.
// Copyright (c) 2026 Abdur Rahim (MIT). Preserve fixtures/LICENSE-DigUp.
#include "runtime/llama_embedding_provider.h"
#include "runtime/model_files.h"
#include <QCryptographicHash>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QtTest>
#include <cmath>

class EmbeddingTest : public QObject
{
  Q_OBJECT
  std::unique_ptr<qctsm::LlamaEmbeddingProvider> provider;
  private slots:
    void initTestCase()
    {
      const auto modelDir = qEnvironmentVariable("QCOPILOTS_TOOL_SEMANTIC_MATCHING_MODEL_DIR");
      QVERIFY2(!modelDir.isEmpty(), "Real-model tests require the locked GGUF. Missing models fail, they do not skip.");
      QString error;
      QVERIFY2(qctsm::ModelFiles(modelDir).verify(nullptr, &error), qPrintable(error));
      qctsm::ModelOptions options;
      options.modelDirectory = modelDir;
      options.backend = QStringLiteral("cpu");
      provider = std::make_unique<qctsm::LlamaEmbeddingProvider>(options);
    }
    void rawTextBatchMatchesImmutableReferences()
    {
      QFile fixture(QStringLiteral(QCTSM_RUNTIME_FIXTURES "/vectors.json"));
      QVERIFY(fixture.open(QIODevice::ReadOnly));
      const auto raw = fixture.readAll();
      QCOMPARE(QCryptographicHash::hash(raw, QCryptographicHash::Sha256).toHex(), QByteArray("0ec9336aa53fb0d5bafa074cd56122e2fd914cb73f080b4f98332872089f6dd8"));
      const auto references = QJsonDocument::fromJson(raw).object().value("vectors").toObject();
      const QStringList texts{
        QStringLiteral("task: search result | query: payment declined error"),
        QStringLiteral("task: search result | query: ইলিশ মাছ রান্না"),
        QStringLiteral("task: search result | query: متى تغلق المكتبة"),
        QStringLiteral("title: Tokyo itinerary | text: Flight to Tokyo Haneda on November 3. Then the train to Kamakura to see the Great Buddha, and back for dinner in Shibuya.")};
      const QStringList names{"query", "query-bn", "query-ar", "document"};
      QString error;
      const auto vectors = provider->embed(texts, nullptr, &error);
      QVERIFY2(error.isEmpty(), qPrintable(error));
      QCOMPARE(vectors.size(), texts.size());
      for (qsizetype i = 0; i < vectors.size(); ++i)
      {
        const auto expected = references.value(names.at(i)).toArray();
        QCOMPARE(vectors.at(i).size(), 768);
        QCOMPARE(expected.size(), 768);
        double square = 0, dot = 0, referenceSquare = 0;
        for (int component = 0; component < 768; ++component)
        {
          const double actual = vectors.at(i).at(component);
          QVERIFY(std::isfinite(actual));
          square += actual * actual;
          dot += actual * expected.at(component).toDouble();
          referenceSquare += std::pow(expected.at(component).toDouble(), 2);
        }
        QVERIFY(std::abs(square - 1.0) < .001);
        const double cosine = dot / std::sqrt(square * referenceSquare);
        qInfo() << names.at(i) << "reference cosine" << cosine;
        QVERIFY2(cosine >= .999, "Actual embedding differs from immutable reference. Do not regenerate fixtures.");
      }
      const auto runtime = provider->modelInfo();
      QVERIFY(runtime.value("loaded").toBool());
      QCOMPARE(runtime.value("backend").toString(), QStringLiteral("cpu"));
      QVERIFY(!runtime.value("gpu_compute_verified").toBool());
    }
    void cancelledAndOversizedRequestsNeverReturnPartialBatch()
    {
      QString error;
      std::atomic_bool cancel{true};
      QVERIFY(provider->embed({"hello"}, &cancel, &error).isEmpty());
      QVERIFY(error.contains("cancelled"));
      cancel.store(false);
      QVERIFY(provider->embed({"valid text", QStringLiteral("token ").repeated(4096)}, &cancel, &error).isEmpty());
      QVERIFY2(error.contains("token limit"), qPrintable(error));
      const auto recovered = provider->embed({QStringLiteral("task: search result | query: 缓冲区与道路相交")}, &cancel, &error);
      QVERIFY2(error.isEmpty(), qPrintable(error));
      QCOMPARE(recovered.size(), 1);
      QCOMPARE(recovered.first().size(), 768);
    }
    void availableGpuRunsStrictInferenceAgainstReference()
    {
      const auto discovery = qctsm::LlamaEmbeddingProvider::devices(QStringLiteral("auto"));
      QCOMPARE(discovery.value("status").toString(), QStringLiteral("succeeded"));
      const auto backend = discovery.value("packaged_backend").toString();
      const auto loadError = discovery.value("gpu_load_error").toString();
      if (backend == QLatin1String("cpu") || !loadError.isEmpty())
      {
        qInfo().noquote() << "GPU inference tested=false reason="
          << (backend == QLatin1String("cpu") ? QStringLiteral("CPU-only package") : loadError);
        return;
      }
      QFile fixture(QStringLiteral(QCTSM_RUNTIME_FIXTURES "/vectors.json"));
      QVERIFY(fixture.open(QIODevice::ReadOnly));
      const auto expected = QJsonDocument::fromJson(fixture.readAll()).object()
        .value("vectors").toObject().value("query").toArray();
      QCOMPARE(expected.size(), 768);
      int tested = 0;
      for (const auto &entry : discovery.value("devices").toArray())
      {
        const auto device = entry.toObject();
        if (!device.value("gpu").toBool() || !device.value("available").toBool()
            || device.value("backend").toString() != backend) continue;
        qctsm::ModelOptions options;
        options.modelDirectory = qEnvironmentVariable("QCOPILOTS_TOOL_SEMANTIC_MATCHING_MODEL_DIR");
        options.backend = QStringLiteral("gpu");
        options.deviceId = device.value("device_id").toString();
        options.deviceName = device.value("name").toString();
        qctsm::LlamaEmbeddingProvider gpu(options);
        QString error;
        const auto actual = gpu.embed({QStringLiteral("task: search result | query: payment declined error")}, nullptr, &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(actual.size(), 1);
        QCOMPARE(actual.first().size(), 768);
        double square = 0, dot = 0, referenceSquare = 0;
        for (int component = 0; component < 768; ++component)
        {
          const double value = actual.first().at(component);
          QVERIFY(std::isfinite(value));
          square += value * value;
          dot += value * expected.at(component).toDouble();
          referenceSquare += std::pow(expected.at(component).toDouble(), 2);
        }
        QVERIFY(std::abs(square - 1.0) < .001);
        const double cosine = dot / std::sqrt(square * referenceSquare);
        QVERIFY2(cosine >= .999, "Actual GPU embedding differs from the immutable CPU reference");
        const auto runtime = gpu.modelInfo();
        QCOMPARE(runtime.value("backend").toString(), backend);
        QVERIFY(runtime.value("loaded").toBool());
        QVERIFY(runtime.value("gpu_compute_verified").toBool());
        QVERIFY(runtime.value("fallback_reason").toString().isEmpty());
        QCOMPARE(runtime.value("device").toObject().value("device_id"), device.value("device_id"));
        QCOMPARE(runtime.value("device").toObject().value("name"), device.value("name"));
        qInfo().noquote() << "GPU inference tested=true backend=" << backend
          << "device=" << options.deviceName << "reference cosine=" << cosine;
        ++tested;
      }
      if (!tested)
        qInfo().noquote() << "GPU inference tested=false reason=no compatible physical GPU with required capability; discovery="
          << QString::fromUtf8(QJsonDocument(discovery).toJson(QJsonDocument::Compact));
    }
};
QTEST_GUILESS_MAIN(EmbeddingTest)
#include "embedding_test.moc"
