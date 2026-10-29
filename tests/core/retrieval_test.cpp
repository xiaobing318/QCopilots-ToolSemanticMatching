// SPDX-License-Identifier: MIT
#include "core/retrieval.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLockFile>
#include <QProcess>
#include <QTemporaryDir>
#include <QTest>
#include <QTextStream>
#include <functional>
#include <sqlite3.h>

using namespace qctsm;

class TestProvider final : public EmbeddingProvider
{
  public:
    QStringList requests;
    QList<int> batchSizes;
    bool fail = false;
    bool invalid = false;
    int invalidIndex = -1;
    std::function<void()> onEmbed;
    QList<QVector<float>> embed(const QStringList &texts, const std::atomic_bool *cancel, QString *error) override
    {
      requests.append(texts);
      batchSizes.append(int(texts.size()));
      if (onEmbed) onEmbed();
      if (cancel && cancel->load()) { *error = "cancelled"; return {}; }
      if (fail) { *error = "fixture service unavailable"; return {}; }
      QList<QVector<float>> result;
      for (const auto &text : texts)
      {
        if (invalid || result.size() == invalidIndex) { result.append({0,0,0}); continue; }
        const auto lower = text.toLower();
        result.append({lower.contains("buffer") || lower.contains(QString::fromUtf8("缓冲")) ? 3.0f : 0.1f,
                       lower.contains("clip") || lower.contains(QString::fromUtf8("裁剪")) ? 3.0f : 0.1f,
                       lower.contains("raster") || lower.contains(QString::fromUtf8("栅格")) ? 3.0f : 0.1f});
      }
      return result;
    }
};

QJsonObject definition(const QString &id, const QString &description)
{
  return {{"record_id",id},{"kind","definition"},{"capability_id",id},{"name",id},{"description",description},
          {"payload",QJsonObject{{"name",id},{"description",description},{"inputSchema",QJsonObject{{"type","object"}}}}}};
}
QJsonObject catalog(QJsonArray records, const QString &revision = "r1")
{
  return {{"schema_version",1},{"source_id","test-qgis"},{"revision",revision},{"records",records}};
}
EmbeddingProfile testProfile()
{
  EmbeddingProfile profile; profile.dimensions=3; profile.revision="fixture-v1"; return profile;
}

class RetrievalTest : public QObject
{
  Q_OBJECT
  private slots:
    void bilingualRetrievalAndOriginalJson()
    {
      QTemporaryDir temporary; QVERIFY(temporary.isValid());
      const QString path=temporary.filePath(QString::fromUtf8("共享 数据库/index.sqlite"));
      auto provider=std::make_shared<TestProvider>();
      RetrievalEngine engine(path,provider,testProfile());
      const QJsonObject invocation{{"binary_id","bin.gdalwarp"},{"confirmed_risk",true},{"client_request_id","NEVER_INDEX_REQUEST_ID"},
        {"arguments",QJsonArray{"-r","lanczos","-ts","30000","30000","C:/private/project/input.tif","C:/private/results/output.tif"}}};
      auto snapshot=catalog({definition("native:buffer","Create a polygon buffer distance around vector features 缓冲"),
                             definition("native:clip","Clip a vector layer by boundary 裁剪"),
                             definition("bin.gdalwarp","Raster resampling 栅格重采样"),
                             QJsonObject{{"record_id","warp-example"},{"kind","example"},{"capability_id","bin.gdalwarp"},{"payload",invocation}}});
      QCOMPARE(engine.syncCatalog(snapshot).value("status").toString(),QString("succeeded"));
      auto result=engine.search({{"query",QString::fromUtf8("道路缓冲区")},{"top_n",2}});
      QCOMPARE(result.value("retrieval_mode").toString(),QString("hybrid"));
      QCOMPARE(result.value("candidates").toArray().first().toObject().value("capability_id").toString(),QString("native:buffer"));
      result=engine.search({{"query","Raster resampling lanczos"},{"top_n",8}});
      int warpCount=0;
      for(const auto &item:result.value("candidates").toArray()) if(item.toObject().value("capability_id")=="bin.gdalwarp") ++warpCount;
      QCOMPARE(warpCount,1);
      const auto original=engine.getRecords({"warp-example"}).value("records").toArray().first().toObject().value("payload").toObject();
      QCOMPARE(original,invocation);
      const QString sent=provider->requests.join('\n');
      QVERIFY(!sent.contains("NEVER_INDEX_REQUEST_ID")); QVERIFY(!sent.contains("confirmed_risk")); QVERIFY(!sent.contains("C:/private"));
      QVERIFY(sent.contains("lanczos")); QVERIFY(sent.contains("30000"));
      QVERIFY(sent.contains("task: search result | query:"));
    }

    void incrementalMetadataAndCrossEngineVisibility()
    {
      QTemporaryDir temporary; auto provider=std::make_shared<TestProvider>(); const QString path=temporary.filePath("shared.sqlite");
      RetrievalEngine first(path,provider,testProfile()), second(path,provider,testProfile());
      auto buffer=definition("native:buffer","Vector buffer");
      auto clip=definition("native:clip","Vector clip");
      const auto snapshot=catalog({buffer,clip});
      QCOMPARE(first.syncCatalog(snapshot).value("status").toString(),QString("succeeded"));
      const int initialCalls=provider->requests.size();
      QCOMPARE(first.syncCatalog(snapshot).value("embedded_records").toInt(),0);
      QCOMPARE(provider->requests.size(),initialCalls);
      QCOMPARE(second.status().value("record_count").toInt(),2);
      buffer.insert("available",false);
      const QJsonObject example{{"record_id","b-example"},{"kind","example"},{"capability_id","native:buffer"},{"payload",QJsonObject{{"algorithm_id","native:buffer"}}}};
      QCOMPARE(first.syncCatalog(catalog({buffer,example},"r2")).value("status").toString(),QString("succeeded"));
      QCOMPARE(second.status().value("available_records").toInt(),0);
      QVERIFY(second.search({{"query","buffer"}}).value("candidates").toArray().isEmpty());
      QVERIFY(second.getRecords({"native:clip"}).value("records").toArray().isEmpty());
    }

    void rawRecordNormalizationIsSharedAndLossless()
    {
      const QList<QPair<QJsonObject,QString>> fixtures{
        {{{"binary_id","bin.gdalwarp"}},"example"},
        {{{"algorithm_id","native:buffer"},{"name","Buffer"},{"description","buffer"},{"parameters",QJsonObject{{"DISTANCE",100}}}},"example"},
        {{{"name","buffer"},{"parameters",QJsonObject{{"type","object"},{"properties",QJsonObject{}}}}},"definition"},
        {{{"name","buffer"},{"inputSchema",QJsonObject{{"type","object"}}}},"definition"},
        {{{"id","native:buffer"},{"provider","native"},{"parameters",QJsonArray{QJsonObject{{"name","INPUT"},{"type","source"}}}},{"outputs",QJsonArray{}}},"definition"},
        {{{"id","bin.gdalwarp"},{"name","gdalwarp"},{"risk","R2"}},"definition"},
        {{{"name","send"},{"payload",QJsonObject{{"x",1}}}},"definition"}
      };
      for(const auto &fixture:fixtures)
      {
        const auto record=normalizeCapabilityRecord(fixture.first);
        QCOMPARE(record.value("kind").toString(),fixture.second);
        QCOMPARE(record.value("payload").toObject(),fixture.first);
        QCOMPARE(normalizeCapabilityRecord(record),record);
        QTemporaryDir temporary;auto provider=std::make_shared<TestProvider>();
        RetrievalEngine engine(temporary.filePath("index.sqlite"),provider,testProfile());
        QCOMPARE(engine.syncCatalog(catalog({fixture.first})).value("status").toString(),QString("succeeded"));
        const auto stored=engine.getRecords({record.value("record_id").toString()}).value("records").toArray().first().toObject();
        QCOMPARE(stored.value("payload").toObject(),fixture.first);
        QCOMPARE(stored.value("kind").toString(),fixture.second);
        QCOMPARE(stored.value("capability_id"),record.value("capability_id"));
      }
      const auto processing=normalizeCapabilityRecord(fixtures[4].first);
      QCOMPARE(processing.value("capability_id").toString(),QString("native:buffer"));
      const QJsonObject explicitDefinition{{"kind","definition"},{"payload",fixtures[0].first}};
      QCOMPARE(normalizeCapabilityRecord(explicitDefinition).value("kind").toString(),QString("definition"));
    }

    void documentBatchesAndAtomicValidation()
    {
      QTemporaryDir temporary; auto provider=std::make_shared<TestProvider>();
      RetrievalEngine engine(temporary.filePath("index.sqlite"),provider,testProfile());
      QJsonArray records;
      for(int i=0;i<10;++i) records.append(definition(QString("native:buffer%1").arg(i),"Vector buffer"));
      QList<QPair<qint64,qint64>> progress;
      const auto indexed=engine.syncCatalog(catalog(records),nullptr,[&](qint64 completed,qint64 total){progress.append({completed,total});});
      QCOMPARE(indexed.value("status").toString(),QString("succeeded"));
      QCOMPARE(indexed.value("embedded_records").toInt(),10);
      QCOMPARE(provider->batchSizes,QList<int>({8,8,4}));
      QCOMPARE(engine.status().value("embedded_records").toInt(),10);
      QCOMPARE(progress,(QList<QPair<qint64,qint64>>{{0,10},{4,10},{8,10},{10,10}}));
      progress.clear();
      QCOMPARE(engine.syncCatalog(catalog(records),nullptr,[&](qint64 completed,qint64 total){progress.append({completed,total});}).value("status").toString(),QString("succeeded"));
      QCOMPARE(progress,(QList<QPair<qint64,qint64>>{{10,10}}));

      // A corrupt final item invalidates the whole batch, never a partial set of its records.
      auto corrupt=std::make_shared<TestProvider>(); corrupt->invalidIndex=7;
      RetrievalEngine other(temporary.filePath("corrupt.sqlite"),corrupt,testProfile());
      QCOMPARE(other.syncCatalog(catalog(records)).value("error_code").toString(),QString("embedding_failed"));
      QCOMPARE(other.status().value("record_count").toInt(),10);
      QCOMPARE(other.status().value("embedded_records").toInt(),0);
    }

    void longDocumentAccumulatesAcrossBatches()
    {
      QTemporaryDir temporary;auto provider=std::make_shared<TestProvider>();
      RetrievalEngine engine(temporary.filePath("index.sqlite"),provider,testProfile());
      const QString description=QString::fromUtf8("缓冲区 buffer 🌍 ").repeated(400);
      const auto record=definition("native:buffer",description);
      const auto result=engine.syncCatalog(catalog({record}));
      QCOMPARE(result.value("status").toString(),QString("succeeded"));
      QCOMPARE(result.value("embedded_records").toInt(),1);
      QVERIFY(provider->batchSizes.size()>1);
      for(int size:provider->batchSizes) QVERIFY(size>=1 && size<=8);
      for(const QString &text:provider->requests)
      {
        for(int i=0;i<text.size();++i)
        {
          if(text.at(i).isHighSurrogate()) { QVERIFY(i+1<text.size());QVERIFY(text.at(++i).isLowSurrogate()); }
          else QVERIFY(!text.at(i).isLowSurrogate());
        }
      }
      QCOMPARE(engine.getRecords({"native:buffer"}).value("records").toArray().first().toObject().value("payload"),record.value("payload"));
    }

    void profileIsolationAndFailureFallback()
    {
      QTemporaryDir temporary; auto provider=std::make_shared<TestProvider>(); const QString path=temporary.filePath("index.sqlite");
      RetrievalEngine first(path,provider,testProfile());
      const auto snapshot=catalog({definition("native:buffer","Vector buffer")});
      QCOMPARE(first.syncCatalog(snapshot).value("status").toString(),QString("succeeded"));
      auto otherProfile=testProfile(); otherProfile.revision="different-weights";
      RetrievalEngine second(path,provider,otherProfile);
      const int calls=provider->requests.size();
      auto response=second.search({{"query","native:buffer"}});
      QCOMPARE(response.value("retrieval_mode").toString(),QString("lexical"));
      QCOMPARE(provider->requests.size(),calls);
      QCOMPARE(second.syncCatalog(snapshot).value("embedded_records").toInt(),1);
      QCOMPARE(first.status().value("embedded_records").toInt(),1);
      QCOMPARE(second.status().value("embedded_records").toInt(),1);
      provider->fail=true;
      response=first.search({{"query","native:buffer"}});
      QCOMPARE(response.value("status").toString(),QString("succeeded"));
      QCOMPARE(response.value("retrieval_mode").toString(),QString("lexical"));
      QVERIFY(response.value("degradation_reason").toString().contains("unavailable"));
      auto changed=definition("native:clip","Vector clip");
      response=first.syncCatalog(catalog({changed},"r2"));
      QCOMPARE(response.value("status").toString(),QString("failed"));
      QVERIFY(response.value("metadata_applied").toBool());
      QVERIFY(first.getRecords({"native:buffer"}).value("records").toArray().isEmpty());
      QCOMPARE(first.status().value("embedded_records").toInt(),0);
    }

    void previousDocumentProfileIsNotReadAsTwoViews()
    {
      QTemporaryDir temporary;const QString path=temporary.filePath("index.sqlite");
      auto provider=std::make_shared<TestProvider>();RetrievalEngine engine(path,provider,testProfile());
      const auto snapshot=catalog({definition("native:buffer","buffer")});
      QCOMPARE(engine.syncCatalog(snapshot).value("status").toString(),QString("succeeded"));
      auto oldProfile=testProfile().toJson();oldProfile.remove("vector_layout");oldProfile.insert("document_version","capability-json-v1");
      const QByteArray profileJson=QJsonDocument(oldProfile).toJson(QJsonDocument::Compact);
      const QByteArray fingerprint=QCryptographicHash::hash(profileJson,QCryptographicHash::Sha256).toHex();
      sqlite3 *raw=nullptr;QCOMPARE(sqlite3_open(path.toUtf8().constData(),&raw),SQLITE_OK);
      sqlite3_stmt *statement=nullptr;
      QCOMPARE(sqlite3_prepare_v2(raw,"INSERT INTO qctsm_profiles VALUES(?,?)",-1,&statement,nullptr),SQLITE_OK);
      QCOMPARE(sqlite3_bind_text(statement,1,fingerprint.constData(),fingerprint.size(),SQLITE_TRANSIENT),SQLITE_OK);
      QCOMPARE(sqlite3_bind_text(statement,2,profileJson.constData(),profileJson.size(),SQLITE_TRANSIENT),SQLITE_OK);
      QCOMPARE(sqlite3_step(statement),SQLITE_DONE);sqlite3_finalize(statement);
      QCOMPARE(sqlite3_prepare_v2(raw,"UPDATE qctsm_vectors SET profile=?,vector=substr(vector,1,12)",-1,&statement,nullptr),SQLITE_OK);
      QCOMPARE(sqlite3_bind_text(statement,1,fingerprint.constData(),fingerprint.size(),SQLITE_TRANSIENT),SQLITE_OK);
      QCOMPARE(sqlite3_step(statement),SQLITE_DONE);sqlite3_finalize(statement);QCOMPARE(sqlite3_close(raw),SQLITE_OK);
      const int previousRequests=provider->requests.size();
      QCOMPARE(engine.search({{"query","buffer"}}).value("retrieval_mode").toString(),QString("lexical"));
      QCOMPARE(provider->requests.size(),previousRequests);
      QCOMPARE(engine.status().value("embedded_records").toInt(),0);
      QCOMPARE(engine.syncCatalog(snapshot).value("embedded_records").toInt(),1);
      QCOMPARE(engine.search({{"query","buffer"}}).value("retrieval_mode").toString(),QString("hybrid"));
    }

    void noTransactionHeldAcrossEmbedding()
    {
      QTemporaryDir temporary; auto provider=std::make_shared<TestProvider>(); const QString path=temporary.filePath("index.sqlite");
      RetrievalEngine first(path,provider,testProfile()), reader(path,nullptr,testProfile());
      bool observed=false;
      provider->onEmbed=[&] {
        const auto state=reader.status();
        observed=state.value("status")=="succeeded" && state.value("record_count").toInt()==1;
        auto competingCatalog=catalog({definition("native:clip","clip")},"other");
        competingCatalog.insert("source_id","another-application");
        const auto competing=reader.syncCatalog(competingCatalog);
        QVERIFY(competing.value("metadata_applied").toBool());
        QCOMPARE(competing.value("error_code").toString(),QString("embedding_failed"));
      };
      QCOMPARE(first.syncCatalog(catalog({definition("native:buffer","buffer")})).value("status").toString(),QString("succeeded"));
      QVERIFY(observed);
    }

    void rawExamplesAndValidation()
    {
      QTemporaryDir temporary; auto provider=std::make_shared<TestProvider>();
      RetrievalEngine engine(temporary.filePath("index.sqlite"),provider,testProfile());
      const QJsonObject raw{{"binary_id","bin.gdalwarp"},{"arguments",QJsonArray{"-r","lanczos"}}};
      QCOMPARE(engine.syncCatalog(catalog({raw})).value("status").toString(),QString("succeeded"));
      QCOMPARE(engine.getRecords({"bin.gdalwarp"}).value("records").toArray().first().toObject().value("payload").toObject(),raw);
      QCOMPARE(engine.syncCatalog(catalog({definition("b","buffer")})).value("error_code").toString(),QString("source_revision_conflict"));
      QCOMPARE(engine.search({{"query",""}}).value("error_code").toString(),QString("invalid_query"));
      QCOMPARE(engine.search({{"query","buffer"},{"top_n",0}}).value("error_code").toString(),QString("invalid_top_n"));
      QCOMPARE(engine.syncCatalog(catalog({definition("same","a"),definition("same","b")},"r3")).value("error_code").toString(),QString("duplicate_record_id"));
      provider->invalid=true;
      QCOMPARE(engine.syncCatalog(catalog({definition("buffer","buffer")},"r4")).value("error_code").toString(),QString("embedding_failed"));
    }

    void walReaderDoesNotWaitForWriter()
    {
      QTemporaryDir temporary;const QString path=temporary.filePath("index.sqlite");
      RetrievalEngine engine(path,nullptr,testProfile());
      QCOMPARE(engine.status().value("status").toString(),QString("succeeded"));
      sqlite3 *writer=nullptr;
      QCOMPARE(sqlite3_open(path.toUtf8().constData(),&writer),SQLITE_OK);
      QCOMPARE(sqlite3_exec(writer,"BEGIN IMMEDIATE; UPDATE qctsm_meta SET value=value+1",nullptr,nullptr,nullptr),SQLITE_OK);
      QElapsedTimer timer;timer.start();
      const auto result=engine.status();
      const auto elapsed=timer.elapsed();
      QCOMPARE(sqlite3_exec(writer,"ROLLBACK",nullptr,nullptr,nullptr),SQLITE_OK);
      QCOMPARE(sqlite3_close(writer),SQLITE_OK);
      QCOMPARE(result.value("status").toString(),QString("succeeded"));
      QVERIFY2(elapsed<1000,"A WAL read waited for an unrelated writer transaction.");
    }

    void deletionDuringQueryEmbeddingIsObserved()
    {
      QTemporaryDir temporary;const QString path=temporary.filePath("index.sqlite");
      auto provider=std::make_shared<TestProvider>();
      RetrievalEngine engine(path,provider,testProfile()),other(path,provider,testProfile());
      QCOMPARE(engine.syncCatalog(catalog({definition("native:buffer","buffer")})).value("status").toString(),QString("succeeded"));
      const auto oldGeneration=engine.status().value("generation").toDouble();
      provider->onEmbed=[&]{QCOMPARE(other.syncCatalog(catalog({},"r2")).value("status").toString(),QString("succeeded"));};
      const auto result=engine.search({{"query","buffer"}});
      QVERIFY(result.value("candidates").toArray().isEmpty());
      QVERIFY(result.value("generation").toDouble()>oldGeneration);
    }

    void supersededEmbeddingCannotPublish()
    {
      QTemporaryDir temporary;const QString path=temporary.filePath("index.sqlite");
      auto provider=std::make_shared<TestProvider>();
      RetrievalEngine engine(path,provider,testProfile()),other(path,nullptr,testProfile());
      provider->onEmbed=[&]{QCOMPARE(other.syncCatalog(catalog({},"r2")).value("status").toString(),QString("succeeded"));};
      const auto result=engine.syncCatalog(catalog({definition("native:buffer","buffer")}));
      QCOMPARE(result.value("error_code").toString(),QString("source_changed"));
      QCOMPARE(other.status().value("record_count").toInt(),0);
      QCOMPARE(other.status().value("embedded_records").toInt(),0);
    }

    void forceRebuildPreservesRecordsAndRevision()
    {
      QTemporaryDir temporary;auto provider=std::make_shared<TestProvider>();
      RetrievalEngine engine(temporary.filePath("index.sqlite"),provider,testProfile());
      auto snapshot=catalog({definition("native:buffer","buffer")});
      QCOMPARE(engine.syncCatalog(snapshot).value("status").toString(),QString("succeeded"));
      const auto before=engine.getRecords({"native:buffer"}).value("records");
      snapshot.insert("force_rebuild",true);
      QCOMPARE(engine.syncCatalog(snapshot).value("embedded_records").toInt(),1);
      QCOMPARE(engine.getRecords({"native:buffer"}).value("records"),before);
      QCOMPARE(engine.syncCatalog(catalog({definition("native:buffer","buffer")})).value("embedded_records").toInt(),0);
      snapshot.insert("force_rebuild","true");
      QCOMPARE(engine.syncCatalog(snapshot).value("error_code").toString(),QString("invalid_catalog"));
    }

    void expectedRevisionRejectsStaleSnapshot()
    {
      QTemporaryDir temporary;auto provider=std::make_shared<TestProvider>();
      RetrievalEngine engine(temporary.filePath("index.sqlite"),provider,testProfile());
      auto initial=catalog({definition("native:buffer","buffer")});
      initial.insert("expected_revision",QJsonValue(QJsonValue::Null));
      QCOMPARE(engine.syncCatalog(initial).value("status").toString(),QString("succeeded"));
      auto next=catalog({definition("native:clip","clip")},"r2");next.insert("expected_revision","r1");
      QCOMPARE(engine.syncCatalog(next).value("status").toString(),QString("succeeded"));
      initial.insert("expected_revision","r1");
      QCOMPARE(engine.syncCatalog(initial).value("error_code").toString(),QString("source_revision_conflict"));
      QCOMPARE(engine.status().value("sources").toArray().first().toObject().value("revision").toString(),QString("r2"));
    }

    void existingDatabaseMustRemainWal()
    {
      QTemporaryDir temporary;const QString path=temporary.filePath("index.sqlite");
      RetrievalEngine engine(path,nullptr,testProfile());
      QCOMPARE(engine.status().value("status").toString(),QString("succeeded"));
      sqlite3 *raw=nullptr;QCOMPARE(sqlite3_open(path.toUtf8().constData(),&raw),SQLITE_OK);
      QCOMPARE(sqlite3_exec(raw,"PRAGMA journal_mode=DELETE",nullptr,nullptr,nullptr),SQLITE_OK);
      QCOMPARE(sqlite3_close(raw),SQLITE_OK);
      const auto result=engine.status();
      QCOMPARE(result.value("status").toString(),QString("failed"));
      QVERIFY(result.value("message").toString().contains("WAL"));
      QVERIFY(QFile::remove(path)); // Constructor failure must not leave a Windows file handle open.
    }

    void crossProcessSnapshotIsObserved()
    {
      QTemporaryDir temporary;const QString path=temporary.filePath(QString::fromUtf8("共享 index.sqlite"));
      auto provider=std::make_shared<TestProvider>();RetrievalEngine engine(path,provider,testProfile());
      QCOMPARE(engine.syncCatalog(catalog({definition("native:buffer","buffer")})).value("status").toString(),QString("succeeded"));
      const auto generation=engine.status().value("generation").toDouble();
      QProcess child;
      child.start(QCoreApplication::applicationFilePath(),{"--replace-catalog",path,"r2","native:clip"});
      QVERIFY(child.waitForStarted());QVERIFY(child.waitForFinished(10000));
      QCOMPARE(child.exitStatus(),QProcess::NormalExit);QCOMPARE(child.exitCode(),0);
      const auto state=engine.status();
      QVERIFY(state.value("generation").toDouble()>generation);
      QCOMPARE(state.value("record_count").toInt(),1);
      QCOMPARE(state.value("sources").toArray().first().toObject().value("revision").toString(),QString("r2"));
      QVERIFY(engine.getRecords({"native:buffer"}).value("records").toArray().isEmpty());
      QCOMPARE(engine.search({{"query","clip"}}).value("candidates").toArray().first().toObject().value("capability_id").toString(),QString("native:clip"));
    }

    void cancellationAndWriterProcess()
    {
      QTemporaryDir temporary; QVERIFY(temporary.isValid());
      const QString path=temporary.filePath(QString::fromUtf8("中文 路径/shared.sqlite"));
      auto provider=std::make_shared<TestProvider>(); RetrievalEngine engine(path,provider,testProfile());
      std::atomic_bool stop{true};
      QCOMPARE(engine.syncCatalog(catalog({definition("buffer","buffer")}),&stop).value("status").toString(),QString("cancelled"));
      QVERIFY(!QFileInfo::exists(path));
      QCOMPARE(engine.status().value("status").toString(),QString("succeeded"));
      QProcess child; child.start(QCoreApplication::applicationFilePath(),{"--hold-index-lock",path});
      QVERIFY(child.waitForStarted()); QVERIFY(child.waitForReadyRead(10000));
      QVERIFY(child.readAllStandardOutput().contains("locked"));
      QCOMPARE(engine.syncCatalog(catalog({definition("buffer","buffer")})).value("error_code").toString(),QString("index_busy"));
      QCOMPARE(engine.status().value("status").toString(),QString("succeeded"));
      child.terminate(); if(!child.waitForFinished(5000)){child.kill();QVERIFY(child.waitForFinished());}
      QCOMPARE(engine.syncCatalog(catalog({definition("buffer","buffer")})).value("status").toString(),QString("succeeded"));
    }
};

int main(int argc,char **argv)
{
  QCoreApplication app(argc,argv);
  if(app.arguments().size()==5 && app.arguments()[1]=="--replace-catalog")
  {
    auto provider=std::make_shared<TestProvider>();RetrievalEngine engine(app.arguments()[2],provider,testProfile());
    const auto result=engine.syncCatalog(catalog({definition(app.arguments()[4],app.arguments()[4])},app.arguments()[3]));
    QTextStream stream(stdout);stream<<QJsonDocument(result).toJson(QJsonDocument::Compact)<<'\n';stream.flush();
    return result.value("status")=="succeeded"?0:3;
  }
  if(app.arguments().size()==3 && app.arguments()[1]=="--hold-index-lock")
  {
    const QFileInfo info(app.arguments()[2]);
    QLockFile lock(QDir(QFileInfo(info.absolutePath()).canonicalFilePath()).filePath(info.fileName()+".writer.lock"));
    lock.setStaleLockTime(0);
    if(!lock.tryLock(0)) return 2;
    QTextStream stream(stdout);stream<<"locked\n";stream.flush();
    return app.exec();
  }
  RetrievalTest test;return QTest::qExec(&test,argc,argv);
}
#include "retrieval_test.moc"
