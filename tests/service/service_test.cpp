// SPDX-License-Identifier: MIT
#include "service/service_config.h"
#include "service/task_service.h"
#include "service/http_server.h"
#include <httplib.h>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QTest>
#include <atomic>
#include <chrono>
#include <thread>

class ServiceTest:public QObject {
  Q_OBJECT
private slots:
  void profileRoundTrip() {
    QTemporaryDir dir;QVERIFY(dir.isValid());QString error;
    auto c=qctsm::ServiceConfig::load(dir.path(),&error);QVERIFY(error.isEmpty());QVERIFY(!c.token.isEmpty());
    c.databasePath=dir.filePath(QStringLiteral("共享工具索引.sqlite3"));c.authorization=QStringLiteral("Bearer test-only");
    QVERIFY(c.save(&error));auto other=qctsm::ServiceConfig::load(dir.path(),&error);QVERIFY(error.isEmpty());
    QCOMPARE(other.databasePath,c.databasePath);QCOMPARE(other.token,c.token);QVERIFY(other.toJson(false).value("token").toString().isEmpty());QVERIFY(other.toJson(false).value("authorization").toString().isEmpty());
  }
  void examplePayloadPreserved() {
    const QJsonObject example{{"binary_id","bin.gdalwarp"},{"confirmed_risk",true},{"client_request_id","example"},{"arguments",QJsonArray{"-r","lanczos","C:/source.tif","C:/target.tif"}}};
    auto catalog=qctsm::normalizeCatalog(example,QStringLiteral("example-source"));const auto record=catalog.value("records").toArray().first().toObject();
    QCOMPARE(record.value("kind").toString(),QStringLiteral("example"));QCOMPARE(record.value("capability_id").toString(),QStringLiteral("bin.gdalwarp"));QCOMPARE(record.value("payload").toObject(),example);
  }
  void embeddedIdentityCannotBeRelabelled() {
    QTemporaryDir dir;QVERIFY(dir.isValid());auto config=qctsm::ServiceConfig::load(dir.path());QString error;
    config.modelRevision=QStringLiteral("another-remote-model");
    QVERIFY(!config.save(&error));QVERIFY(error.contains(QStringLiteral("locked model manifest")));
    config.provider=QStringLiteral("http");QVERIFY(config.save(&error));
    config.apiBase=QStringLiteral("https://user:secret@example.org/v1/");QVERIFY(!config.save(&error));
  }
  void rawArrayAndExplicitCatalogPreserved() {
    const QJsonObject distanceSchema{{"type","number"}};
    const QJsonObject parametersSchema{{"type","object"},{"properties",QJsonObject{{"DISTANCE",distanceSchema}}}};
    const QJsonObject definition{{"name","native:buffer"},{"description","Create a buffer"},{"parameters",parametersSchema}};
    const QJsonObject example{{"algorithm_id","native:buffer"},{"parameters",QJsonObject{{"DISTANCE",100}}}};
    const auto catalog=qctsm::normalizeCatalog(QJsonArray{definition,example},QStringLiteral("tools"));
    const auto records=catalog.value("records").toArray();QCOMPARE(records.size(),2);
    QCOMPARE(records[0].toObject().value("kind").toString(),QStringLiteral("definition"));
    QCOMPARE(records[1].toObject().value("kind").toString(),QStringLiteral("example"));
    QCOMPARE(records[0].toObject().value("payload").toObject(),definition);
    QCOMPARE(records[1].toObject().value("payload").toObject(),example);
    auto explicitCatalog=catalog;explicitCatalog["expected_revision"]=QJsonValue::Null;explicitCatalog["force_rebuild"]=true;
    QCOMPARE(qctsm::normalizeCatalog(explicitCatalog,QStringLiteral("tools")),explicitCatalog);
    const auto empty=qctsm::normalizeCatalog(QJsonArray{},QStringLiteral("tools"));
    QVERIFY(empty.value("records").toArray().isEmpty());QVERIFY(!empty.value("revision").toString().isEmpty());
    QCOMPARE(qctsm::normalizeCatalog(QJsonArray{3},QStringLiteral("tools")).value("error_code").toString(),QStringLiteral("invalid_catalog"));
    const QJsonObject rawPayload{{"name","send"},{"payload",QJsonObject{{"x",1}}}};
    const QJsonObject rawRecords{{"name","count_records"},{"arguments",QJsonObject{}},{"records",QJsonArray{QJsonObject{{"id",1}}}}};
    for(const auto &raw:{rawPayload,rawRecords}) {
      const auto normalized=qctsm::normalizeCatalog(raw,QStringLiteral("raw-fields"));
      QCOMPARE(normalized.value("records").toArray().first().toObject().value("payload").toObject(),raw);
    }
  }
  void largeResultsSurviveRestart() {
    QTemporaryDir dir;QVERIFY(dir.isValid());auto config=qctsm::ServiceConfig::load(dir.path());
    QVERIFY(QDir().mkpath(dir.filePath(QStringLiteral("job-results"))));
    const auto resultPath=dir.filePath(QStringLiteral("job-results/large-result.json"));
    QFile result(resultPath);QVERIFY(result.open(QIODevice::WriteOnly));
    const int length=33*1024*1024;
    result.write(QJsonDocument(QJsonObject{{"status","succeeded"},{"blob",QString(length,QLatin1Char('x'))}}).toJson(QJsonDocument::Compact));result.close();
    QFile journal(dir.filePath(QStringLiteral("jobs.json")));QVERIFY(journal.open(QIODevice::WriteOnly));
    journal.write(QJsonDocument(QJsonObject{{"schema_version",1},{"jobs",QJsonArray{QJsonObject{{"job_id","large-result"},{"operation","search"},{"status","succeeded"},{"result_file","large-result.json"}}}}}).toJson());journal.close();
    qctsm::TaskService service(config);const auto restored=service.snapshot(QStringLiteral("large-result"));
    QCOMPARE(restored.value("status").toString(),QStringLiteral("succeeded"));
    QCOMPARE(restored.value("result").toObject().value("blob").toString().size(),length);
    QVERIFY(QFileInfo(journal.fileName()).size()<32*1024);
    QVERIFY(QFile::remove(resultPath));
    const auto missing=service.snapshot(QStringLiteral("large-result"));
    QCOMPARE(missing.value("error_code").toString(),QStringLiteral("job_result_unavailable"));
    QCOMPARE(missing.value("execution_status").toString(),QStringLiteral("succeeded"));service.stop();
  }
  void interruptedJobIsNotReplayed() {
    QTemporaryDir dir;QVERIFY(dir.isValid());auto config=qctsm::ServiceConfig::load(dir.path());
    QFile file(dir.filePath(QStringLiteral("jobs.json")));QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(QJsonDocument(QJsonObject{{"schema_version",1},{"jobs",QJsonArray{QJsonObject{{"job_id","interrupted-index"},{"operation","sync_catalog"},{"status","running"}}}}}).toJson());file.close();
    qctsm::TaskService service(config);const auto restored=service.snapshot(QStringLiteral("interrupted-index"));
    QCOMPARE(restored.value("status").toString(),QStringLiteral("failed"));
    QCOMPARE(restored.value("error_code").toString(),QStringLiteral("interrupted"));
    QVERIFY(!QFile::exists(config.databasePath));service.stop();
  }
  void cancelStopsInFlightEmbedding() {
    struct Remote {
      httplib::Server server;
      std::atomic_bool started{false};
      std::thread thread;
      int port=0;
      Remote() {
        server.Post("/v1/embeddings",[this](const httplib::Request &request,httplib::Response &response){
          started=true;
          // Deliberately provide no vector. Cancellation must abort this request.
          const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
          while(!request.is_connection_closed()&&std::chrono::steady_clock::now()<deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
          response.status=503;
        });
        port=server.bind_to_any_port("127.0.0.1");
        if(port>0)thread=std::thread([this]{server.listen_after_bind();});
      }
      ~Remote(){server.stop();if(thread.joinable())thread.join();}
    } remote;
    QVERIFY(remote.port>0);
    QTemporaryDir dir;QVERIFY(dir.isValid());auto config=qctsm::ServiceConfig::load(dir.path());
    config.provider=QStringLiteral("http");config.apiBase=QStringLiteral("http://127.0.0.1:%1/v1/").arg(remote.port);
    qctsm::TaskService service(config);
    const auto catalog=qctsm::normalizeCatalog(QJsonObject{{"binary_id","bin.gdalwarp"},{"arguments",QJsonArray{"-r","lanczos"}}},QStringLiteral("cancel-source"));
    const auto queued=service.submit(QStringLiteral("sync_catalog"),catalog);
    const auto id=queued.value("job_id").toString();QVERIFY(!id.isEmpty());
    QTRY_VERIFY_WITH_TIMEOUT(remote.started.load(),3000);
    service.cancel(id);
    QTRY_COMPARE_WITH_TIMEOUT(service.snapshot(id).value("status").toString(),QStringLiteral("cancelled"),3000);
    QCOMPARE(service.indexStatus().value("embedded_records").toInt(),0);
    service.stop();
  }
  void httpBoundaries() {
    QTemporaryDir dir;QVERIFY(dir.isValid());auto config=qctsm::ServiceConfig::load(dir.path());config.port=0;config.token=QStringLiteral("test-service-secret");
    auto service=std::make_shared<qctsm::TaskService>(config);qctsm::HttpServer server(config,service);QString error;QVERIFY2(server.start(&error),qPrintable(error));
    httplib::Client client("127.0.0.1",server.port());client.set_connection_timeout(5);client.set_read_timeout(5);
    auto health=client.Get("/health");QVERIFY(health);QCOMPARE(health->status,200);
    auto denied=client.Get("/v1/models");QVERIFY(denied);QCOMPARE(denied->status,401);
    client.set_default_headers({{"Authorization","Bearer test-service-secret"}});
    auto models=client.Get("/v1/models");QVERIFY(models);QCOMPARE(models->status,200);
    auto input=client.Post("/v1/search","{\"query\":\"\"}","application/json");QVERIFY(input);QCOMPARE(input->status,400);
    auto model=client.Post("/v1/embeddings","{\"model\":\"unknown\",\"input\":\"hello\"}","application/json");QVERIFY(model);QCOMPARE(model->status,400);
    auto missing=client.Get("/v1/jobs/not-a-job");QVERIFY(missing);QCOMPARE(missing->status,404);
    for(const auto &path:{"/v1/catalogs/geo%2520tools","/v1/catalogs/geo%20tools","/v1/catalogs/geo%2Ftools"}) {
      auto imported=client.Put(path,"[]","application/json");QVERIFY(imported);QCOMPARE(imported->status,202);
    }
    QTRY_COMPARE_WITH_TIMEOUT(service->indexStatus().value("sources").toArray().size(),3,3000);
    QStringList sources;for(const auto &value:service->indexStatus().value("sources").toArray())sources<<value.toObject().value("source_id").toString();
    QVERIFY(sources.contains(QStringLiteral("geo%20tools")));QVERIFY(sources.contains(QStringLiteral("geo tools")));QVERIFY(sources.contains(QStringLiteral("geo/tools")));
    auto csrf=client.Post("/v1/shutdown",{{"Origin","https://example.com"}},"{}","application/json");QVERIFY(csrf);QCOMPARE(csrf->status,403);QVERIFY(!server.shutdownRequested());
    auto stop=client.Post("/v1/shutdown","{}","application/json");QVERIFY(stop);QCOMPARE(stop->status,200);QVERIFY(server.shutdownRequested());
    service->stop();server.stop();
  }
};
QTEST_GUILESS_MAIN(ServiceTest)
#include "service_test.moc"
