// SPDX-License-Identifier: MIT
#include "main_window.h"
#include <QApplication>
#include <QClipboard>
#include <QDir>
#include <QFileInfo>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QTableWidget>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTest>

namespace {
class Endpoint : public QTcpServer
{
  public:
    int searches = 0;
    int polls = 0;
    bool finish = true;
    QJsonObject query;
    QByteArray authorization;
    QJsonObject original{{"binary_id", "bin.gdalwarp"}, {"client_request_id", "keep-original"}, {"confirmed_risk", true},
      {"arguments", QJsonArray{"-r", "lanczos", "C:/源文件/输入.tif", "C:/结果/输出.tif"}}};
    explicit Endpoint(QObject *parent = nullptr) : QTcpServer(parent)
    {
      connect(this, &QTcpServer::newConnection, this, [this] {
        while (auto *socket = nextPendingConnection()) {
          socket->setProperty("buffer", QByteArray());
          connect(socket, &QTcpSocket::readyRead, socket, [this, socket] {
            QByteArray buffer = socket->property("buffer").toByteArray() + socket->readAll();
            const auto split = buffer.indexOf("\r\n\r\n");
            if (split < 0) { socket->setProperty("buffer", buffer); return; }
            int length = 0;
            const auto headers = buffer.left(split).split('\n');
            for (const auto &raw : headers) {
              const auto line = raw.trimmed();
              if (line.toLower().startsWith("content-length:")) length = line.mid(15).trimmed().toInt();
            }
            if (buffer.size() < split + 4 + length) { socket->setProperty("buffer", buffer); return; }
            const auto parts = headers.first().trimmed().split(' ');
            const auto method = parts.value(0);
            const auto path = parts.value(1);
            QJsonObject response;
            if (path == "/health") response = {{"status", "ok"}, {"service", "qctsm"}};
            else if (path == "/v1/models") response = {{"object", "list"}, {"data", QJsonArray{QJsonObject{{"id", "embeddinggemma-2-Q8_0"}}}}};
            else if (path == "/v1/index/status") response = {{"status", "succeeded"}, {"generation", 1}, {"record_count", 1}};
            else if (path == "/v1/model/info") response = {{"status", "succeeded"}, {"provider", "http"}, {"model", "embeddinggemma-2-Q8_0"}, {"revision", "test"}};
            else if (path == "/v1/search" && method == "POST") {
              ++searches;
              query = QJsonDocument::fromJson(buffer.mid(split + 4, length)).object();
              for (const auto &line : headers) if (line.toLower().startsWith("authorization:")) authorization = line.mid(14).trimmed();
              response = {{"status", "queued"}, {"job_id", "gui-job"}};
            } else if (path == "/v1/jobs/gui-job") {
              ++polls;
              response = {{"status", finish ? "succeeded" : "running"}, {"job_id", "gui-job"}};
              if (finish) response.insert("result", QJsonObject{{"status", "succeeded"}, {"generation", 1}, {"retrieval_mode", "hybrid"},
                {"candidates", QJsonArray{QJsonObject{{"record_id", "record-1"}, {"capability_id", "bin.gdalwarp"}, {"name", "Warp"},
                  {"score", 0.03}, {"semantic_score", 0.8}, {"payload", original}}}}});
            } else response = {{"status", "failed"}, {"message", "unexpected path"}};
            const auto body = QJsonDocument(response).toJson(QJsonDocument::Compact);
            socket->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\nContent-Length: " + QByteArray::number(body.size()) + "\r\n\r\n" + body);
            socket->disconnectFromHost();
          });
          connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
        }
      });
    }
};

qctsm::ServiceConfig configFor(const QString &directory, quint16 port)
{
  auto config = qctsm::ServiceConfig::load(directory);
  config.port = port;
  config.token = QStringLiteral("gui-test-token");
  config.modelRevision = QStringLiteral("test");
  return config;
}
}

class GuiTest : public QObject
{
  Q_OBJECT
  private slots:
    void queryPreservesOriginalJson()
    {
      QTemporaryDir temporary;
      QVERIFY(temporary.isValid());
      Endpoint endpoint;
      QVERIFY(endpoint.listen(QHostAddress::LocalHost, 0));
      qctsm::MainWindow window(configFor(temporary.path(), endpoint.serverPort()));
      window.show();
      auto *query = window.findChild<QPlainTextEdit *>(QStringLiteral("searchQuery"));
      auto *search = window.findChild<QPushButton *>(QStringLiteral("searchButton"));
      auto *table = window.findChild<QTableWidget *>(QStringLiteral("candidateTable"));
      QVERIFY(query && search && table);
      query->setPlainText(QStringLiteral("将栅格重采样为新的分辨率"));
      search->click();
      QTRY_VERIFY2_WITH_TIMEOUT(table->rowCount() == 1, qPrintable(window.findChild<QLabel *>(QStringLiteral("notification"))->text()), 5000);
      QCOMPARE(endpoint.query.value("query").toString(), query->toPlainText());
      QCOMPARE(endpoint.query.value("top_n").toInt(), 8);
      QCOMPARE(endpoint.authorization, QByteArray("Bearer gui-test-token"));
      QVERIFY(search->isEnabled());
      window.findChild<QPushButton *>(QStringLiteral("copyOriginalJson"))->click();
      QCOMPARE(QJsonDocument::fromJson(QApplication::clipboard()->text().toUtf8()).object(), endpoint.original);
      QVERIFY(!QFileInfo::exists(QDir(temporary.path()).filePath(QStringLiteral("config.json"))));
    }

    void editingSettingsKeepsRunningJobConnection()
    {
      QTemporaryDir temporary;
      QVERIFY(temporary.isValid());
      Endpoint original;
      Endpoint other;
      original.finish = false;
      QVERIFY(original.listen(QHostAddress::LocalHost, 0));
      QVERIFY(other.listen(QHostAddress::LocalHost, 0));
      qctsm::MainWindow window(configFor(temporary.path(), original.serverPort()));
      window.show();
      // Let the initial health check establish a verified service connection.
      QTest::qWait(200);
      window.findChild<QPlainTextEdit *>(QStringLiteral("searchQuery"))->setPlainText(QStringLiteral("buffer a road"));
      window.findChild<QPushButton *>(QStringLiteral("searchButton"))->click();
      QTRY_VERIFY_WITH_TIMEOUT(original.polls > 0, 5000);
      window.findChild<QSpinBox *>(QStringLiteral("servicePort"))->setValue(other.serverPort());
      window.findChild<QLineEdit *>(QStringLiteral("serviceToken"))->setText(QStringLiteral("different-token"));
      window.findChild<QPushButton *>(QStringLiteral("saveSettings"))->click();
      QString error;
      const auto saved = qctsm::ServiceConfig::load(temporary.path(), &error);
      QVERIFY2(error.isEmpty(), qPrintable(error));
      QCOMPARE(saved.port, int(other.serverPort()));
      QCOMPARE(saved.token, QStringLiteral("different-token"));
      const auto before = original.polls;
      original.finish = true;
      QTRY_VERIFY_WITH_TIMEOUT(original.polls > before, 5000);
      QTRY_VERIFY2_WITH_TIMEOUT(window.findChild<QTableWidget *>(QStringLiteral("candidateTable"))->rowCount() == 1, qPrintable(window.findChild<QLabel *>(QStringLiteral("notification"))->text()), 5000);
      QCOMPARE(other.polls, 0);
      QCOMPARE(other.searches, 0);
    }
};

QTEST_MAIN(GuiTest)
#include "gui_test.moc"
