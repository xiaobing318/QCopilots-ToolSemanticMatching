// SPDX-License-Identifier: MIT
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QTemporaryDir>
#include <QtTest>

class WorkerProtocolTest : public QObject
{
  Q_OBJECT
  QProcess worker;
  QJsonObject receive()
  {
    QElapsedTimer elapsed;
    elapsed.start();
    while (!worker.canReadLine() && elapsed.elapsed() < 5000 && worker.state() != QProcess::NotRunning)
      worker.waitForReadyRead(100);
    const auto line = worker.readLine();
    QJsonParseError error;
    const auto json = QJsonDocument::fromJson(line, &error);
    if (error.error != QJsonParseError::NoError || !json.isObject())
      QTest::qFail(qPrintable(QStringLiteral("Worker returned invalid JSON: %1, stderr: %2")
        .arg(QString::fromUtf8(line), QString::fromUtf8(worker.readAllStandardError()))), __FILE__, __LINE__);
    return json.object();
  }
  void send(const QJsonObject &request)
  {
    const auto line = QJsonDocument(request).toJson(QJsonDocument::Compact) + '\n';
    QCOMPARE(worker.write(line), qint64(line.size()));
    if (worker.bytesToWrite() > 0) QVERIFY(worker.waitForBytesWritten(5000));
  }
  private slots:
    void cleanup()
    {
      if (worker.state() != QProcess::NotRunning)
      {
        worker.write("{\"id\":\"cleanup\",\"operation\":\"quit\"}\n");
        if (!worker.waitForFinished(5000)) { worker.kill(); QVERIFY(worker.waitForFinished(5000)); }
      }
    }
    void modelInformationAndFailuresDoNotRequireWeights()
    {
      QTemporaryDir modelDirectory;
      QVERIFY(modelDirectory.isValid());
      worker.setProgram(QStringLiteral(QCTSM_WORKER_EXECUTABLE));
      worker.setArguments({"--model-dir", modelDirectory.path(), "--backend", "cpu"});
      worker.start();
      QVERIFY2(worker.waitForStarted(5000), qPrintable(worker.errorString()));
      send({{"id", "info"}, {"operation", "model_info"}});
      auto reply = receive();
      QCOMPARE(reply.value("id").toString(), QStringLiteral("info"));
      QCOMPARE(reply.value("status").toString(), QStringLiteral("succeeded"));
      QVERIFY(!reply.value("runtime").toObject().value("loaded").toBool());
      QCOMPARE(reply.value("runtime").toObject().value("dimensions").toInt(), 768);
      send({{"id", "wrong-model"}, {"operation", "embed"}, {"model", "wrong"}, {"input", "text"}});
      reply = receive();
      QCOMPARE(reply.value("error_code").toString(), QStringLiteral("unknown_model"));
      send({{"id", "missing-weights"}, {"operation", "embed"}, {"input", "text"}});
      reply = receive();
      QCOMPARE(reply.value("status").toString(), QStringLiteral("failed"));
      QVERIFY(!reply.contains("data"));
      QVERIFY(!reply.value("message").toString().isEmpty());
      send({{"id", "quit"}, {"operation", "quit"}});
      reply = receive();
      QCOMPARE(reply.value("id").toString(), QStringLiteral("quit"));
      if (worker.state() != QProcess::NotRunning) QVERIFY(worker.waitForFinished(5000));
      QCOMPARE(worker.exitStatus(), QProcess::NormalExit);
      QCOMPARE(worker.exitCode(), 0);
      QVERIFY(modelDirectory.remove());
    }
};
QTEST_GUILESS_MAIN(WorkerProtocolTest)
#include "worker_protocol_test.moc"
