// SPDX-License-Identifier: MIT
#include "runtime/model_files.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTimer>
#include <QtTest>

class ModelFilesTest : public QObject
{
  Q_OBJECT
  private slots:
    void lockedManifestContainsOnlyTextModel()
    {
      const auto models = qctsm::ModelFiles::manifest().value("models").toArray();
      QCOMPARE(models.size(), 1);
      const auto model = models.at(0).toObject();
      QCOMPARE(model.value("dimensions").toInt(), 768);
      QVERIFY(!model.value("supportsVision").toBool());
      const auto artifact = qctsm::ModelFiles::defaultArtifact();
      QCOMPARE(artifact.name, QStringLiteral("embeddinggemma-2-Q8_0.gguf"));
      QCOMPARE(artifact.size, qint64(309855456));
      QCOMPARE(artifact.sha256, QByteArray("2188ac1deca4b77dffefd603c2776a9d76d9d74ec01841392982ebb840b09135"));
      QCOMPARE(model.value("artifacts").toArray().size(), 1);
    }
    void verifiedImportIsAtomicAndCancellable()
    {
      QTemporaryDir temporary;
      QVERIFY(temporary.isValid());
      const QByteArray content("test artifact, not an inference model");
      const auto source = temporary.filePath(QStringLiteral("模型 source.gguf"));
      {
        QFile file(source);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write(content), content.size());
      }
      qctsm::ModelArtifact artifact{"fixture.gguf", QUrl("https://example.invalid/fixture.gguf"), content.size(),
        QCryptographicHash::hash(content, QCryptographicHash::Sha256).toHex()};
      qctsm::ModelFiles files(temporary.filePath(QStringLiteral("模型 destination")), artifact);
      QString error;
      std::atomic_bool cancel{true};
      QVERIFY(!files.importFile(source, &cancel, &error));
      QVERIFY(error.contains("cancelled"));
      QVERIFY(!QFileInfo::exists(files.filePath()));
      cancel.store(false);
      qint64 received = 0;
      QVERIFY2(files.importFile(source, &cancel, &error, [&](qint64 size, qint64) { received = size; }), qPrintable(error));
      QCOMPARE(received, qint64(content.size()));
      QVERIFY2(files.verify(&cancel, &error), qPrintable(error));
      {
        QFile corrupted(source);
        QVERIFY(corrupted.open(QIODevice::WriteOnly));
        QCOMPARE(corrupted.write(QByteArray(content.size(), 'x')), content.size());
      }
      QVERIFY(!files.importFile(source, &cancel, &error));
      QVERIFY(error.contains("SHA-256"));
      QVERIFY2(files.verify(&cancel, &error), qPrintable(error));
      QVERIFY(files.importFile(files.filePath(), &cancel, &error));
      QVERIFY(temporary.remove());
    }
    void rejectsUnsafeManifest()
    {
      QTemporaryDir temporary;
      QVERIFY(temporary.isValid());
      auto artifact = qctsm::ModelFiles::defaultArtifact();
      artifact.name = QStringLiteral("../outside.gguf");
      QString error;
      QVERIFY(!qctsm::ModelFiles(temporary.path(), artifact).verify(nullptr, &error));
      QVERIFY(error.contains("Invalid"));
      QVERIFY(temporary.remove());
    }
    void downloadChecksHashBeforePublicationAndCanCancel()
    {
      QTemporaryDir temporary;
      QVERIFY(temporary.isValid());
      const QByteArray content("download fixture, not an inference model");
      QByteArray responseContent(content.size(), 'x');
      bool stall = false;
      QTcpServer server;
      QVERIFY(server.listen(QHostAddress::LocalHost, 0));
      connect(&server, &QTcpServer::newConnection, &server, [&] {
        while (server.hasPendingConnections())
        {
          auto *socket = server.nextPendingConnection();
          connect(socket, &QTcpSocket::readyRead, socket, [&, socket] {
            auto request = socket->property("request").toByteArray() + socket->readAll();
            socket->setProperty("request", request);
            if (!request.contains("\r\n\r\n") || socket->property("sent").toBool()) return;
            socket->setProperty("sent", true);
            if (stall) return;
            socket->write("HTTP/1.1 200 OK\r\nConnection: close\r\nContent-Length: "
                          + QByteArray::number(responseContent.size()) + "\r\n\r\n" + responseContent);
            socket->disconnectFromHost();
          });
        }
      });
      qctsm::ModelArtifact artifact{"fixture.gguf",
        QUrl(QStringLiteral("http://127.0.0.1:%1/fixture.gguf").arg(server.serverPort())), content.size(),
        QCryptographicHash::hash(content, QCryptographicHash::Sha256).toHex()};
      qctsm::ModelFiles files(temporary.filePath("download"), artifact);
      QString error;
      QVERIFY(!files.downloadMissing(nullptr, &error));
      QVERIFY2(error.contains("SHA-256"), qPrintable(error));
      QVERIFY(!QFileInfo::exists(files.filePath()));
      responseContent = content;
      QVERIFY2(files.downloadMissing(nullptr, &error), qPrintable(error));
      QVERIFY2(files.verify(nullptr, &error), qPrintable(error));
      server.close();
      // A valid existing model can be used without a network connection.
      QVERIFY2(files.downloadMissing(nullptr, &error), qPrintable(error));
      QVERIFY(server.listen(QHostAddress::LocalHost, 0));
      artifact.url = QUrl(QStringLiteral("http://127.0.0.1:%1/fixture.gguf").arg(server.serverPort()));
      qctsm::ModelFiles cancelledFiles(temporary.filePath("cancelled-download"), artifact);
      stall = true;
      std::atomic_bool cancel{false};
      QTimer::singleShot(25, &server, [&] { cancel.store(true); });
      QVERIFY(!cancelledFiles.downloadMissing(&cancel, &error));
      QVERIFY2(error.contains("cancelled"), qPrintable(error));
      QVERIFY(!QFileInfo::exists(cancelledFiles.filePath()));
      QVERIFY(temporary.remove());
    }
};
QTEST_GUILESS_MAIN(ModelFilesTest)
#include "model_files_test.moc"
