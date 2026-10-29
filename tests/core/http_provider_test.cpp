// SPDX-License-Identifier: MIT
#include "provider_http/http_embedding_provider.h"

#include <QHostAddress>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTest>
#include <QTimer>

#include <atomic>

namespace
{
class LoopbackServer : public QObject
{
  public:
    explicit LoopbackServer(QObject *parent = nullptr) : QObject(parent)
    {
      connect(&server, &QTcpServer::newConnection, this, [this]() {
        while (QTcpSocket *socket = server.nextPendingConnection())
        {
          ++connections;
          connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
          connect(socket, &QTcpSocket::readyRead, this, [this, socket]() {
            if (socket->property("handled").toBool())
              return;
            QByteArray buffer = socket->property("buffer").toByteArray();
            buffer += socket->readAll();
            socket->setProperty("buffer", buffer);
            const qsizetype headerEnd = buffer.indexOf("\r\n\r\n");
            if (headerEnd < 0)
              return;
            const QList<QByteArray> lines = buffer.left(headerEnd).split('\n');
            headers.clear();
            for (const QByteArray &line : lines.mid(1))
            {
              const qsizetype colon = line.indexOf(':');
              if (colon > 0)
                headers.insert(line.left(colon).trimmed().toLower(), line.mid(colon + 1).trimmed());
            }
            const qint64 contentLength = headers.value("content-length").toLongLong();
            if (buffer.size() - headerEnd - 4 < contentLength)
              return;
            path = lines.front().split(' ').value(1);
            body = buffer.mid(headerEnd + 4, contentLength);
            socket->setProperty("handled", true);
            ++requests;
            if (noResponse || (noResponseFromRequest > 0 && requests >= noResponseFromRequest))
              return;
            if (disconnectsRemaining > 0)
            {
              --disconnectsRemaining;
              socket->abort();
              return;
            }
            const int responseStatus = failuresRemaining > 0 ? transientStatus : status;
            if (failuresRemaining > 0)
              --failuresRemaining;
            QByteArray result = "HTTP/1.1 " + QByteArray::number(responseStatus) + " Response\r\n";
            result += "Content-Type: application/json\r\nConnection: close\r\n";
            if (!location.isEmpty())
              result += "Location: " + location + "\r\n";
            if (!retryAfter.isEmpty())
              result += "Retry-After: " + retryAfter + "\r\n";
            result += "Content-Length: " + QByteArray::number(response.size()) + "\r\n\r\n";
            result += response;
            socket->write(result);
            socket->disconnectFromHost();
          });
        }
      });
      listening = server.listen(QHostAddress::LocalHost, 0);
    }

    QUrl apiBase() const
    {
      return QUrl(QStringLiteral("http://127.0.0.1:%1/prefix/v1").arg(server.serverPort()));
    }

    QTcpServer server;
    bool listening = false;
    bool noResponse = false;
    int status = 200;
    int connections = 0;
    int requests = 0;
    int failuresRemaining = 0;
    int transientStatus = 503;
    int disconnectsRemaining = 0;
    int noResponseFromRequest = 0;
    QByteArray response = R"({"data":[{"index":1,"embedding":[0,1]},{"index":0,"embedding":[1,0]}]})";
    QByteArray location;
    QByteArray retryAfter;
    QByteArray path;
    QByteArray body;
    QMap<QByteArray, QByteArray> headers;
};
}

class HttpProviderTest : public QObject
{
    Q_OBJECT
  private slots:
    void sortedBatchPreservesPayloadAndPrefix()
    {
      LoopbackServer server;
      QVERIFY(server.listening);
      qctsm::HttpEmbeddingProvider provider(server.apiBase(), QStringLiteral("example-model"), "Bearer test", 1000);
      provider.setRequestHeaders({{"X-API-Key", "another-test-key"}});
      QString error;
      const QStringList texts{QStringLiteral("task: search result | query: 道路缓冲"), QStringLiteral("title: Clip | text: Clip vectors")};
      const auto result = provider.embed(texts, nullptr, &error);
      QVERIFY2(error.isEmpty(), qPrintable(error));
      QCOMPARE(result.size(), 2);
      QCOMPARE(result.at(0), QVector<float>({1, 0}));
      QCOMPARE(result.at(1), QVector<float>({0, 1}));
      QCOMPARE(server.path, QByteArray("/prefix/v1/embeddings"));
      QCOMPARE(server.headers.value("authorization"), QByteArray("Bearer test"));
      QCOMPARE(server.headers.value("x-api-key"), QByteArray("another-test-key"));
      const auto sent = QJsonDocument::fromJson(server.body).object();
      QCOMPARE(sent.value(QStringLiteral("model")).toString(), QStringLiteral("example-model"));
      QCOMPARE(sent.value(QStringLiteral("encoding_format")).toString(), QStringLiteral("float"));
      QCOMPARE(sent.value(QStringLiteral("input")).toArray(), QJsonArray::fromStringList(texts));
    }

    void invalidResponses_data()
    {
      QTest::addColumn<QByteArray>("response");
      QTest::addColumn<QString>("expectedPrefix");
      QTest::newRow("bad-json") << QByteArray("not json") << QStringLiteral("invalid_json:");
      QTest::newRow("wrong-root") << QByteArray("[]") << QStringLiteral("invalid_json:");
      QTest::newRow("count") << QByteArray(R"({"data":[]})") << QStringLiteral("invalid_batch:");
      QTest::newRow("object") << QByteArray(R"({"data":[1,2]})") << QStringLiteral("invalid_batch:");
      QTest::newRow("duplicates") << QByteArray(R"({"data":[{"index":0,"embedding":[1,0]},{"index":0,"embedding":[0,1]}]})") << QStringLiteral("duplicate_index:");
      QTest::newRow("missing-index") << QByteArray(R"({"data":[{"embedding":[1,0]},{"index":1,"embedding":[0,1]}]})") << QStringLiteral("invalid_index:");
      QTest::newRow("negative-index") << QByteArray(R"({"data":[{"index":-1,"embedding":[1,0]},{"index":1,"embedding":[0,1]}]})") << QStringLiteral("invalid_index:");
      QTest::newRow("fractional-index") << QByteArray(R"({"data":[{"index":0.5,"embedding":[1,0]},{"index":1,"embedding":[0,1]}]})") << QStringLiteral("invalid_index:");
      QTest::newRow("range-index") << QByteArray(R"({"data":[{"index":2,"embedding":[1,0]},{"index":1,"embedding":[0,1]}]})") << QStringLiteral("invalid_index:");
      QTest::newRow("base64") << QByteArray(R"({"data":[{"index":0,"embedding":"base64"},{"index":1,"embedding":[0,1]}]})") << QStringLiteral("invalid_vector:");
      QTest::newRow("empty") << QByteArray(R"({"data":[{"index":0,"embedding":[]},{"index":1,"embedding":[0,1]}]})") << QStringLiteral("invalid_dimensions:");
      QTest::newRow("dimensions") << QByteArray(R"({"data":[{"index":0,"embedding":[1]},{"index":1,"embedding":[0,1]}]})") << QStringLiteral("invalid_dimensions:");
      QTest::newRow("text-number") << QByteArray(R"({"data":[{"index":0,"embedding":["1",0]},{"index":1,"embedding":[0,1]}]})") << QStringLiteral("invalid_vector:");
      QTest::newRow("null-number") << QByteArray(R"({"data":[{"index":0,"embedding":[null,0]},{"index":1,"embedding":[0,1]}]})") << QStringLiteral("invalid_vector:");
      QTest::newRow("float-overflow") << QByteArray(R"({"data":[{"index":0,"embedding":[1e100,0]},{"index":1,"embedding":[0,1]}]})") << QStringLiteral("invalid_vector:");
      QTest::newRow("zero") << QByteArray(R"({"data":[{"index":0,"embedding":[0,0]},{"index":1,"embedding":[0,1]}]})") << QStringLiteral("zero_vector:");
    }

    void invalidResponses()
    {
      QFETCH(QByteArray, response);
      QFETCH(QString, expectedPrefix);
      LoopbackServer server;
      QVERIFY(server.listening);
      server.response = response;
      qctsm::HttpEmbeddingProvider provider(server.apiBase(), QStringLiteral("model"), {}, 1000);
      QString error;
      QVERIFY(provider.embed({QStringLiteral("one"), QStringLiteral("two")}, nullptr, &error).isEmpty());
      QVERIFY2(error.startsWith(expectedPrefix), qPrintable(error));
      QCOMPARE(server.requests, 1);
    }

    void transientHttpErrorsRetried_data()
    {
      QTest::addColumn<int>("status");
      QTest::newRow("rate-limit") << 429;
      QTest::newRow("bad-gateway") << 502;
      QTest::newRow("unavailable") << 503;
      QTest::newRow("gateway-timeout") << 504;
    }

    void transientHttpErrorsRetried()
    {
      QFETCH(int, status);
      LoopbackServer server;
      QVERIFY(server.listening);
      server.failuresRemaining = 2;
      server.transientStatus = status;
      qctsm::HttpEmbeddingProvider provider(server.apiBase(), QStringLiteral("model"), {}, 2500);
      QString error;
      const auto result = provider.embed({QStringLiteral("one"), QStringLiteral("two")}, nullptr, &error);
      QVERIFY2(error.isEmpty(), qPrintable(error));
      QCOMPARE(result.size(), 2);
      QCOMPARE(server.requests, 3);
    }

    void persistentTransientErrorHasThreeAttemptLimit()
    {
      LoopbackServer server;
      QVERIFY(server.listening);
      server.status = 503;
      qctsm::HttpEmbeddingProvider provider(server.apiBase(), QStringLiteral("model"), {}, 2500);
      QString error;
      QVERIFY(provider.embed({QStringLiteral("one")}, nullptr, &error).isEmpty());
      QVERIFY2(error.startsWith(QStringLiteral("http_503:")), qPrintable(error));
      QCOMPARE(server.requests, 3);
    }

    void authenticationErrorNotRetried()
    {
      LoopbackServer server;
      QVERIFY(server.listening);
      server.status = 401;
      qctsm::HttpEmbeddingProvider provider(server.apiBase(), QStringLiteral("model"), {}, 2500);
      QString error;
      QVERIFY(provider.embed({QStringLiteral("one")}, nullptr, &error).isEmpty());
      QVERIFY2(error.startsWith(QStringLiteral("http_401:")), qPrintable(error));
      QCOMPARE(server.requests, 1);
    }

    void cancellationDuringBackoff()
    {
      LoopbackServer server;
      QVERIFY(server.listening);
      server.status = 503;
      std::atomic_bool cancel = false;
      QTimer::singleShot(50, this, [&cancel]() { cancel.store(true); });
      qctsm::HttpEmbeddingProvider provider(server.apiBase(), QStringLiteral("model"), {}, 2500);
      QString error;
      QVERIFY(provider.embed({QStringLiteral("one")}, &cancel, &error).isEmpty());
      QVERIFY2(error.startsWith(QStringLiteral("cancelled:")), qPrintable(error));
      QCOMPARE(server.requests, 1);
    }

    void retriesShareOneDeadline()
    {
      LoopbackServer server;
      QVERIFY(server.listening);
      server.failuresRemaining = 1;
      server.noResponseFromRequest = 2;
      qctsm::HttpEmbeddingProvider provider(server.apiBase(), QStringLiteral("model"), {}, 350);
      QElapsedTimer elapsed;
      elapsed.start();
      QString error;
      QVERIFY(provider.embed({QStringLiteral("one")}, nullptr, &error).isEmpty());
      QVERIFY2(error.startsWith(QStringLiteral("timeout:")), qPrintable(error));
      QCOMPARE(server.requests, 2);
      QVERIFY(elapsed.elapsed() < 800);
    }

    void longRetryAfterDoesNotExtendDeadline()
    {
      LoopbackServer server;
      QVERIFY(server.listening);
      server.status = 429;
      server.retryAfter = "60";
      qctsm::HttpEmbeddingProvider provider(server.apiBase(), QStringLiteral("model"), {}, 1000);
      QString error;
      QVERIFY(provider.embed({QStringLiteral("one")}, nullptr, &error).isEmpty());
      QVERIFY2(error.startsWith(QStringLiteral("http_429:")), qPrintable(error));
      QCOMPARE(server.requests, 1);
    }

    void remoteConnectionCloseRecovers()
    {
      LoopbackServer server;
      QVERIFY(server.listening);
      server.disconnectsRemaining = 2;
      qctsm::HttpEmbeddingProvider provider(server.apiBase(), QStringLiteral("model"), {}, 2500);
      QString error;
      const auto result = provider.embed({QStringLiteral("one"), QStringLiteral("two")}, nullptr, &error);
      QVERIFY2(error.isEmpty(), qPrintable(error));
      QCOMPARE(result.size(), 2);
      QCOMPARE(server.requests, 3);
    }

    void httpErrorPreserved()
    {
      LoopbackServer server;
      QVERIFY(server.listening);
      server.status = 429;
      server.response = R"({"error":{"message":"server busy"}})";
      qctsm::HttpEmbeddingProvider provider(server.apiBase(), QStringLiteral("model"), {}, 1000);
      QString error;
      QVERIFY(provider.embed({QStringLiteral("one")}, nullptr, &error).isEmpty());
      QVERIFY2(error.startsWith(QStringLiteral("http_429:")), qPrintable(error));
      QVERIFY(error.contains(QStringLiteral("server busy")));
    }

    void timeoutCancelsNetwork()
    {
      LoopbackServer server;
      QVERIFY(server.listening);
      server.noResponse = true;
      qctsm::HttpEmbeddingProvider provider(server.apiBase(), QStringLiteral("model"), {}, 80);
      QString error;
      QVERIFY(provider.embed({QStringLiteral("one")}, nullptr, &error).isEmpty());
      QVERIFY2(error.startsWith(QStringLiteral("timeout:")), qPrintable(error));
      QCOMPARE(server.connections, 1);
    }

    void cancellationAbortsRequest()
    {
      LoopbackServer server;
      QVERIFY(server.listening);
      server.noResponse = true;
      std::atomic_bool cancel = false;
      QTimer::singleShot(30, this, [&cancel]() { cancel.store(true); });
      qctsm::HttpEmbeddingProvider provider(server.apiBase(), QStringLiteral("model"), {}, 1000);
      QString error;
      QVERIFY(provider.embed({QStringLiteral("one")}, &cancel, &error).isEmpty());
      QVERIFY2(error.startsWith(QStringLiteral("cancelled:")), qPrintable(error));
    }

    void cancelledBeforeSubmission()
    {
      LoopbackServer server;
      QVERIFY(server.listening);
      std::atomic_bool cancel = true;
      qctsm::HttpEmbeddingProvider provider(server.apiBase(), QStringLiteral("model"));
      QString error;
      QVERIFY(provider.embed({QStringLiteral("one")}, &cancel, &error).isEmpty());
      QVERIFY(error.startsWith(QStringLiteral("cancelled:")));
      QCOMPARE(server.connections, 0);
    }

    void emptyBatchMakesNoRequest()
    {
      LoopbackServer server;
      QVERIFY(server.listening);
      qctsm::HttpEmbeddingProvider provider(server.apiBase(), QStringLiteral("model"));
      QString error = QStringLiteral("previous error");
      QVERIFY(provider.embed({}, nullptr, &error).isEmpty());
      QVERIFY(error.isEmpty());
      QCOMPARE(server.connections, 0);
    }

    void crossOriginRedirectDoesNotForwardCredentials()
    {
      LoopbackServer source;
      LoopbackServer target;
      QVERIFY(source.listening && target.listening);
      source.status = 307;
      source.location = target.apiBase().toEncoded() + "/embeddings";
      source.response.clear();
      qctsm::HttpEmbeddingProvider provider(source.apiBase(), QStringLiteral("model"), "Bearer secret", 1000);
      QString error;
      QVERIFY(provider.embed({QStringLiteral("one")}, nullptr, &error).isEmpty());
      QVERIFY(!error.isEmpty());
      QCOMPARE(target.connections, 0);
    }

    void rejectsInjectedHeaders()
    {
      LoopbackServer server;
      QVERIFY(server.listening);
      qctsm::HttpEmbeddingProvider provider(server.apiBase(), QStringLiteral("model"));
      provider.setRequestHeaders({{"X-API-Key", "key\r\nInjected: value"}});
      QString error;
      QVERIFY(provider.embed({QStringLiteral("one")}, nullptr, &error).isEmpty());
      QVERIFY(error.startsWith(QStringLiteral("invalid_headers:")));
      QCOMPARE(server.connections, 0);
    }
};

QTEST_GUILESS_MAIN(HttpProviderTest)
#include "http_provider_test.moc"
