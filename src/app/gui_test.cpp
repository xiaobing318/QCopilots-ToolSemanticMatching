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
#include <QNetworkProxy>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QSpinBox>
#include <QTableWidget>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <functional>

namespace {
class Endpoint : public QTcpServer
{
  public:
    int searches = 0;
    int polls = 0;
    int modelRequests = 0;
    int indexRequests = 0;
    int unauthorizedRequests = 0;
    bool finish = true;
    QJsonObject query;
    QByteArray authorization;
    QByteArray expectedAuthorization = "Bearer gui-test-token";
    std::function<bool(const QByteArray &, const QByteArray &, const QJsonObject &, QTcpSocket *)> customHandler;
    QJsonObject original{{"binary_id", "bin.gdalwarp"}, {"client_request_id", "keep-original"}, {"confirmed_risk", true},
      {"arguments", QJsonArray{"-r", "lanczos", "C:/源文件/输入.tif", "C:/结果/输出.tif"}}};
    static void reply(QTcpSocket *socket, const QJsonObject &response, int status = 200)
    {
      if (!socket || socket->state() == QAbstractSocket::UnconnectedState) return;
      const auto body = QJsonDocument(response).toJson(QJsonDocument::Compact);
      const auto statusText = status == 200 ? QByteArray("200 OK") : QByteArray::number(status) + " Error";
      socket->write("HTTP/1.1 " + statusText + "\r\nContent-Type: application/json\r\nConnection: close\r\nContent-Length: " + QByteArray::number(body.size()) + "\r\n\r\n" + body);
      socket->disconnectFromHost();
    }
    QJsonObject completed(const QString &job, const QString &capability = QStringLiteral("bin.gdalwarp")) const
    {
      return {{"status", "succeeded"}, {"job_id", job}, {"result", QJsonObject{{"status", "succeeded"}, {"generation", 1}, {"retrieval_mode", "hybrid"},
        {"candidates", QJsonArray{QJsonObject{{"record_id", "record-1"}, {"capability_id", capability}, {"name", "Warp"},
          {"score", 0.03}, {"semantic_score", 0.8}, {"payload", original}}}}}}};
    }
    explicit Endpoint(QObject *parent = nullptr) : QTcpServer(parent)
    {
      connect(this, &QTcpServer::newConnection, this, [this] {
        while (auto *socket = nextPendingConnection()) {
          socket->setProperty("buffer", QByteArray());
          connect(socket, &QTcpSocket::readyRead, socket, [this, socket] {
            if (socket->property("handled").toBool()) return;
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
            socket->setProperty("handled", true);
            const auto parts = headers.first().trimmed().split(' ');
            const auto method = parts.value(0);
            const auto path = parts.value(1);
            QByteArray requestAuthorization;
            for (const auto &raw : headers) {
              const auto line = raw.trimmed();
              if (line.toLower().startsWith("authorization:")) requestAuthorization = line.mid(14).trimmed();
            }
            if (path != "/health" && requestAuthorization != expectedAuthorization) {
              ++unauthorizedRequests;
              reply(socket, {{"status", "failed"}, {"error_code", "unauthorized"}, {"message", "A valid service bearer token is required"}}, 401);
              return;
            }
            const auto requestBody = QJsonDocument::fromJson(buffer.mid(split + 4, length)).object();
            if (path == "/v1/search" && method == "POST") {
              ++searches;
              query = requestBody;
              authorization = requestAuthorization;
            }
            if (path.startsWith("/v1/jobs/") && method == "GET") ++polls;
            if (customHandler && customHandler(method, path, requestBody, socket)) return;
            QJsonObject response;
            if (path == "/health") response = {{"status", "ok"}, {"service", "qctsm"}};
            else if (path == "/v1/models") {
              ++modelRequests;
              response = {{"object", "list"}, {"data", QJsonArray{QJsonObject{{"id", "embeddinggemma-2-Q8_0"}}}}};
            } else if (path == "/v1/index/status") {
              ++indexRequests;
              response = {{"status", "succeeded"}, {"generation", 1}, {"record_count", 1}};
            }
            else if (path == "/v1/model/info") response = {{"status", "succeeded"}, {"provider", "http"}, {"model", "embeddinggemma-2-Q8_0"}, {"revision", "test"}};
            else if (path == "/v1/search" && method == "POST") {
              response = {{"status", "queued"}, {"job_id", "gui-job"}};
            } else if (path == "/v1/jobs/gui-job") {
              response = finish ? completed(QStringLiteral("gui-job")) : QJsonObject{{"status", "running"}, {"job_id", "gui-job"}};
            } else response = {{"status", "failed"}, {"message", "unexpected path"}};
            reply(socket, response);
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
  config.provider = QStringLiteral("http");
  config.apiBase = QStringLiteral("http://127.0.0.1:%1/v1/").arg(port);
  config.modelRevision = QStringLiteral("test");
  return config;
}
}

class GuiTest : public QObject
{
  Q_OBJECT
  private slots:
    void initTestCase()
    {
      // These tests own their loopback servers and must not inherit a host proxy.
      QNetworkProxy::setApplicationProxy(QNetworkProxy(QNetworkProxy::NoProxy));
    }
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
      QTRY_VERIFY_WITH_TIMEOUT(original.indexRequests > 0, 5000);
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

    void correctedTokenCanReconnectWithoutRestart()
    {
      QTemporaryDir temporary;
      QVERIFY(temporary.isValid());
      Endpoint endpoint;
      QVERIFY(endpoint.listen(QHostAddress::LocalHost, 0));
      auto config = configFor(temporary.path(), endpoint.serverPort());
      config.token = QStringLiteral("incorrect-token");
      qctsm::MainWindow window(config);
      window.show();
      QTRY_VERIFY_WITH_TIMEOUT(endpoint.unauthorizedRequests > 0, 5000);
      QCOMPARE(endpoint.modelRequests, 0);
      window.findChild<QLineEdit *>(QStringLiteral("serviceToken"))->setText(QStringLiteral("gui-test-token"));
      window.findChild<QPushButton *>(QStringLiteral("saveSettings"))->click();
      window.findChild<QPushButton *>(QStringLiteral("checkConnection"))->click();
      QTRY_VERIFY_WITH_TIMEOUT(endpoint.indexRequests > 0, 5000);
      window.findChild<QPlainTextEdit *>(QStringLiteral("searchQuery"))->setPlainText(QStringLiteral("重采样 raster"));
      window.findChild<QPushButton *>(QStringLiteral("searchButton"))->click();
      QTRY_COMPARE_WITH_TIMEOUT(window.findChild<QTableWidget *>(QStringLiteral("candidateTable"))->rowCount(), 1, 5000);
      QCOMPARE(endpoint.authorization, QByteArray("Bearer gui-test-token"));
    }

    void delayedSubmissionKeepsItsOriginalConnection()
    {
      QTemporaryDir temporary;
      QVERIFY(temporary.isValid());
      Endpoint original;
      Endpoint other;
      other.expectedAuthorization = "Bearer other-token";
      QVERIFY(original.listen(QHostAddress::LocalHost, 0));
      QVERIFY(other.listen(QHostAddress::LocalHost, 0));
      QPointer<QTcpSocket> submission;
      original.customHandler = [&submission](const QByteArray &method, const QByteArray &path, const QJsonObject &, QTcpSocket *socket) {
        if (method == "POST" && path == "/v1/search") { submission = socket; return true; }
        return false;
      };
      qctsm::MainWindow window(configFor(temporary.path(), original.serverPort()));
      window.show();
      QTRY_VERIFY_WITH_TIMEOUT(original.indexRequests > 0, 5000);
      auto *search = window.findChild<QPushButton *>(QStringLiteral("searchButton"));
      window.findChild<QPlainTextEdit *>(QStringLiteral("searchQuery"))->setPlainText(QStringLiteral("warp the raster"));
      search->click();
      QTRY_VERIFY_WITH_TIMEOUT(!submission.isNull(), 5000);
      QVERIFY(!search->isEnabled());
      window.findChild<QSpinBox *>(QStringLiteral("servicePort"))->setValue(other.serverPort());
      window.findChild<QLineEdit *>(QStringLiteral("serviceToken"))->setText(QStringLiteral("other-token"));
      window.findChild<QPushButton *>(QStringLiteral("saveSettings"))->click();
      window.findChild<QPushButton *>(QStringLiteral("checkConnection"))->click();
      QTRY_VERIFY_WITH_TIMEOUT(other.indexRequests > 0, 5000);
      Endpoint::reply(submission, {{"status", "queued"}, {"job_id", "gui-job"}});
      QTRY_COMPARE_WITH_TIMEOUT(window.findChild<QTableWidget *>(QStringLiteral("candidateTable"))->rowCount(), 1, 5000);
      QVERIFY(original.polls > 0);
      QCOMPARE(other.polls, 0);
      QCOMPARE(other.searches, 0);
      QVERIFY(search->isEnabled());
    }

    void cancelledSearchCannotOverwriteANewerSearch()
    {
      QTemporaryDir temporary;
      QVERIFY(temporary.isValid());
      Endpoint endpoint;
      QVERIFY(endpoint.listen(QHostAddress::LocalHost, 0));
      QPointer<QTcpSocket> firstPoll;
      QPointer<QTcpSocket> secondPoll;
      int cancellations = 0;
      endpoint.customHandler = [&](const QByteArray &method, const QByteArray &path, const QJsonObject &, QTcpSocket *socket) {
        if (method == "POST" && path == "/v1/search") {
          Endpoint::reply(socket, {{"status", "queued"}, {"job_id", endpoint.searches == 1 ? QStringLiteral("job-a") : QStringLiteral("job-b")}});
          return true;
        }
        if (method == "GET" && path == "/v1/jobs/job-a") { firstPoll = socket; return true; }
        if (method == "GET" && path == "/v1/jobs/job-b") { secondPoll = socket; return true; }
        if (method == "POST" && path == "/v1/jobs/job-a/cancel") {
          ++cancellations;
          Endpoint::reply(socket, {{"status", "cancelled"}, {"job_id", "job-a"}});
          return true;
        }
        return false;
      };
      qctsm::MainWindow window(configFor(temporary.path(), endpoint.serverPort()));
      window.show();
      QTRY_VERIFY_WITH_TIMEOUT(endpoint.indexRequests > 0, 5000);
      auto *query = window.findChild<QPlainTextEdit *>(QStringLiteral("searchQuery"));
      auto *search = window.findChild<QPushButton *>(QStringLiteral("searchButton"));
      auto *jobs = window.findChild<QTableWidget *>(QStringLiteral("jobTable"));
      auto *candidates = window.findChild<QTableWidget *>(QStringLiteral("candidateTable"));
      query->setPlainText(QStringLiteral("first query"));
      search->click();
      QTRY_VERIFY_WITH_TIMEOUT(!firstPoll.isNull(), 5000);
      QCOMPARE(jobs->rowCount(), 1);
      jobs->selectRow(0);
      window.findChild<QPushButton *>(QStringLiteral("cancelJob"))->click();
      QTRY_VERIFY_WITH_TIMEOUT(search->isEnabled(), 5000);
      QCOMPARE(cancellations, 1);
      query->setPlainText(QStringLiteral("second query"));
      search->click();
      QTRY_VERIFY_WITH_TIMEOUT(!secondPoll.isNull(), 5000);
      QVERIFY(!search->isEnabled());
      // Deliver the pre-cancellation snapshot only after B is already waiting.
      // The client must ignore this terminal A response and leave B pending.
      QTimer::singleShot(0, &endpoint, [&] { Endpoint::reply(firstPoll, endpoint.completed(QStringLiteral("job-a"), QStringLiteral("old-result"))); });
      QTRY_VERIFY_WITH_TIMEOUT(firstPoll.isNull(), 5000);
      QTest::qWait(100);
      QVERIFY(!search->isEnabled());
      QCOMPARE(candidates->rowCount(), 0);
      QCOMPARE(jobs->item(0, 1)->text(), QStringLiteral("已取消"));
      Endpoint::reply(secondPoll, endpoint.completed(QStringLiteral("job-b"), QStringLiteral("new-result")));
      QTRY_COMPARE_WITH_TIMEOUT(candidates->rowCount(), 1, 5000);
      QCOMPARE(candidates->item(0, 0)->text(), QStringLiteral("new-result"));
      QVERIFY(search->isEnabled());
    }
};

QTEST_MAIN(GuiTest)
#include "gui_test.moc"
