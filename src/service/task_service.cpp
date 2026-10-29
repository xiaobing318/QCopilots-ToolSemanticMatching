// SPDX-License-Identifier: MIT
#include "task_service.h"
#include "worker_broker.h"
#include "provider_http/http_embedding_provider.h"
#include "runtime/model_files.h"
#include <QCryptographicHash>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <QUrl>
#include <QUuid>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <thread>

namespace qctsm {
namespace {
QJsonObject fail(const QString &code,const QString &message) {return {{"status","failed"},{"error_code",code},{"message",message}};}
QString now(){return QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);}
bool terminal(const QString &s){return s==QStringLiteral("succeeded")||s==QStringLiteral("failed")||s==QStringLiteral("cancelled");}
}
QJsonObject normalizeCatalog(const QJsonValue &value,const QString &sourceId) {
  if(value.isObject() && value.toObject().contains("records")) {auto o=value.toObject();o["source_id"]=sourceId;return o;}
  QJsonArray input=value.isArray()?value.toArray():QJsonArray{value};
  QJsonArray records;
  for(const auto &v:input) {
    if(!v.isObject()) return fail(QStringLiteral("invalid_catalog"),QStringLiteral("Every catalog record must be a JSON object"));
    auto o=v.toObject();
    if(o.contains("payload")) {records.append(o);continue;}
    const auto id=o.value("binary_id").toString(o.value("algorithm_id").toString(o.value("name").toString()));
    const auto hash=QString::fromLatin1(QCryptographicHash::hash(QJsonDocument(o).toJson(QJsonDocument::Compact),QCryptographicHash::Sha256).toHex());
    const auto parameters=o.value("parameters").toObject();
    const bool schema=o.contains("inputSchema")||o.contains("outputSchema")||
      parameters.value("type").toString()==QStringLiteral("object")||parameters.contains("properties");
    const bool example=!schema&&(o.contains("arguments")||o.contains("parameters"));
    QJsonObject r{{"record_id",hash},{"capability_id",id.isEmpty()?hash:id},{"kind",example?"example":"definition"},{"payload",o}};
    if(o.contains("name")) r["name"]=o["name"];
    if(o.contains("description")) r["description"]=o["description"];
    records.append(r);
  }
  return {{"schema_version",1},{"source_id",sourceId},{"revision",QUuid::createUuid().toString(QUuid::WithoutBraces)},{"records",records}};
}
class TaskService::Impl {
public:
  struct Job {
    QString id,operation,status=QStringLiteral("queued"),created=now(),updated=created;
    QJsonObject payload,result;
    std::atomic_bool cancelled{false};
    qint64 completed=0,total=0;
  };
  ServiceConfig config;
  EmbeddingProfile profile;
  std::shared_ptr<WorkerBroker> broker;
  std::shared_ptr<EmbeddingProvider> interactive,indexing;
  mutable std::mutex mutex;
  std::condition_variable available;
  std::map<QString,std::shared_ptr<Job>> jobs;
  std::deque<std::shared_ptr<Job>> queryQueue,indexQueue;
  std::atomic_bool stopping{false};
  std::thread queryThread,indexThread;
  void persist() {
    QJsonArray rows;
    for(const auto &pair:jobs) rows.append(json(*pair.second));
    QSaveFile file(QDir(config.profilePath).filePath(QStringLiteral("jobs.json")));
    if(!QDir().mkpath(config.profilePath)||!file.open(QIODevice::WriteOnly)){qWarning()<<"Cannot persist task journal:"<<file.errorString();return;}
    const auto bytes=QJsonDocument(QJsonObject{{"schema_version",1},{"jobs",rows}}).toJson(QJsonDocument::Compact);
    if(file.write(bytes)!=bytes.size()||!file.commit())qWarning()<<"Cannot commit task journal:"<<file.errorString();
  }
  void restore() {
    QFile file(QDir(config.profilePath).filePath(QStringLiteral("jobs.json")));
    if(!file.open(QIODevice::ReadOnly)||file.size()>32*1024*1024)return;
    const auto rows=QJsonDocument::fromJson(file.readAll()).object().value("jobs").toArray();
    for(const auto &value:rows){
      const auto o=value.toObject();const auto id=o.value("job_id").toString();if(id.isEmpty())continue;
      auto j=std::make_shared<Job>();j->id=id;j->operation=o.value("operation").toString();j->status=o.value("status").toString();j->created=o.value("created_at").toString();j->updated=o.value("updated_at").toString();j->result=o.value("result").toObject();
      if(!terminal(j->status)){j->status=QStringLiteral("failed");j->updated=now();j->result=fail(QStringLiteral("interrupted"),QStringLiteral("The service stopped before this operation completed. Inspect the shared index and submit a new request."));}
      jobs.emplace(id,j);if(jobs.size()>=256)break;
    }
    persist();
  }
  explicit Impl(const ServiceConfig &c):config(c) {
    profile.modelId=c.modelId;profile.revision=c.modelRevision;
    if(c.provider==QStringLiteral("embedded")) {
      broker=std::make_shared<WorkerBroker>(c);
      interactive=std::make_shared<WorkerEmbeddingProvider>(broker,10);
      indexing=std::make_shared<WorkerEmbeddingProvider>(broker,0);
    } else {
      auto api=QUrl(c.apiBase);QString path=api.path();if(!path.endsWith('/')){path+='/';api.setPath(path);}
      interactive=std::make_shared<HttpEmbeddingProvider>(api,c.modelId,c.authorization.toUtf8(),c.timeoutMs);
      indexing=std::make_shared<HttpEmbeddingProvider>(api,c.modelId,c.authorization.toUtf8(),c.timeoutMs);
    }
    restore();queryThread=std::thread([this]{run(false);});indexThread=std::thread([this]{run(true);});
  }
  QJsonObject json(const Job &j) const {
    QJsonObject o{{"job_id",j.id},{"operation",j.operation},{"status",j.status},{"created_at",j.created},{"updated_at",j.updated}};
    if(j.total>0) {o["completed"]=double(j.completed);o["total"]=double(j.total);}
    if(!j.result.isEmpty()) o["result"]=j.result;
    if(j.result.contains("error_code")) {o["error_code"]=j.result.value("error_code");o["message"]=j.result.value("message");}
    return o;
  }
  void run(bool index) {
    while(!stopping.load()) {
      std::shared_ptr<Job> j;
      {
        std::unique_lock lock(mutex);auto &queue=index?indexQueue:queryQueue;
        available.wait(lock,[&]{return stopping.load()||!queue.empty();});
        if(stopping.load())break;
        j=queue.front();queue.pop_front();
        if(j->cancelled.load()) {j->status=QStringLiteral("cancelled");j->updated=now();persist();continue;}
        j->status=QStringLiteral("running");j->updated=now();persist();
      }
      QJsonObject result;
      try {
        if(j->operation==QStringLiteral("model_download") || j->operation==QStringLiteral("model_import")) {
          ModelFiles files(QFileInfo(config.modelPath).absolutePath());QString error;
          const auto progress=[this,j](qint64 completed,qint64 total){std::lock_guard lock(mutex);j->completed=completed;j->total=total;j->updated=now();};
          bool ok=false;
          if(QFileInfo(config.modelPath).fileName()!=ModelFiles::defaultArtifact().name)error=QStringLiteral("Configure the model path with the locked filename embeddinggemma-2-Q8_0.gguf before downloading or importing.");
          else ok=j->operation==QStringLiteral("model_download")?files.downloadMissing(&j->cancelled,&error,progress):files.importFile(j->payload.value("path").toString(),&j->cancelled,&error,progress);
          result=ok?QJsonObject{{"status","succeeded"},{"model_path",config.modelPath}}:fail(QStringLiteral("model_file_error"),error);
        } else {
          RetrievalEngine engine(config.databasePath,index?indexing:interactive,profile);
          if(j->operation==QStringLiteral("sync_catalog")) result=engine.syncCatalog(j->payload,&j->cancelled);
          else result=engine.search(j->payload,&j->cancelled);
        }
      } catch(const std::exception &e){result=fail(QStringLiteral("operation_exception"),QString::fromUtf8(e.what()));}
      catch(...){result=fail(QStringLiteral("operation_exception"),QStringLiteral("Unknown operation exception"));}
      {
        std::lock_guard lock(mutex);
        j->result=result;j->updated=now();
        if(j->cancelled.load() || result.value("status").toString()==QStringLiteral("cancelled")) j->status=QStringLiteral("cancelled");
        else j->status=result.value("status").toString()==QStringLiteral("succeeded")?QStringLiteral("succeeded"):QStringLiteral("failed");
        persist();
      }
    }
  }
};
TaskService::TaskService(const ServiceConfig &c):d(std::make_unique<Impl>(c)){}
TaskService::~TaskService(){stop();}
QJsonObject TaskService::submit(const QString &operation,const QJsonObject &payload) {
  if(operation!=QStringLiteral("search")&&operation!=QStringLiteral("sync_catalog")&&operation!=QStringLiteral("model_download")&&operation!=QStringLiteral("model_import")) return fail(QStringLiteral("unknown_operation"),operation);
  std::lock_guard lock(d->mutex);
  if(d->stopping.load())return fail(QStringLiteral("service_stopping"),QStringLiteral("Service is stopping"));
  if(d->jobs.size()>=256) {
    for(auto it=d->jobs.begin();it!=d->jobs.end()&&d->jobs.size()>=200;) {
      if(terminal(it->second->status))it=d->jobs.erase(it);else ++it;
    }
    if(d->jobs.size()>=256)return fail(QStringLiteral("queue_full"),QStringLiteral("Too many active jobs"));
  }
  auto j=std::make_shared<Impl::Job>();j->id=QUuid::createUuid().toString(QUuid::WithoutBraces);j->operation=operation;j->payload=payload;
  d->jobs.emplace(j->id,j);
  if(operation==QStringLiteral("search"))d->queryQueue.push_back(j);else d->indexQueue.push_back(j);
  d->persist();d->available.notify_all();return d->json(*j);
}
QJsonObject TaskService::snapshot(const QString &id) const {
  std::lock_guard lock(d->mutex);auto it=d->jobs.find(id);return it==d->jobs.end()?fail(QStringLiteral("job_not_found"),id):d->json(*it->second);
}
QJsonObject TaskService::cancel(const QString &id) {
  std::lock_guard lock(d->mutex);auto it=d->jobs.find(id);if(it==d->jobs.end())return fail(QStringLiteral("job_not_found"),id);
  auto &j=*it->second;
  if(!terminal(j.status)){j.cancelled=true;j.status=j.status==QStringLiteral("queued")?QStringLiteral("cancelled"):QStringLiteral("cancelling");j.updated=now();}
  d->persist();d->available.notify_all();return d->json(j);
}
QJsonObject TaskService::getRecords(const QStringList &ids) {RetrievalEngine e(d->config.databasePath,d->interactive,d->profile);return e.getRecords(ids);}
QJsonObject TaskService::indexStatus(){RetrievalEngine e(d->config.databasePath,d->interactive,d->profile);return e.status();}
QJsonObject TaskService::modelInfo() const {
  return {{"status","succeeded"},{"provider",d->config.provider},{"model",d->config.modelId},{"revision",d->config.modelRevision},{"dimensions",768},{"model_file_exists",QFileInfo::exists(d->config.modelPath)},{"model_path",d->config.modelPath},{"backend",d->config.backend},{"manifest",ModelFiles::manifest()}};
}
QJsonObject TaskService::embeddings(const QJsonObject &body,const std::atomic_bool *cancel) {
  if(body.value("model").toString()!=d->config.modelId)return fail(QStringLiteral("model_not_found"),QStringLiteral("Select the model returned by /v1/models"));
  if(body.contains("encoding_format")&&body.value("encoding_format").toString()!=QStringLiteral("float"))return fail(QStringLiteral("unsupported_encoding"),QStringLiteral("Only float encoding is supported"));
  QStringList texts;const auto input=body.value("input");
  if(input.isString())texts<<input.toString();
  else if(input.isArray())for(const auto &v:input.toArray()){if(!v.isString())return fail(QStringLiteral("invalid_input"),QStringLiteral("input must contain strings"));texts<<v.toString();}
  else return fail(QStringLiteral("invalid_input"),QStringLiteral("input must be a string or string array"));
  if(texts.isEmpty()||texts.size()>128)return fail(QStringLiteral("invalid_input"),QStringLiteral("input must contain 1 to 128 strings"));
  for(const auto &text:texts)if(text.isEmpty()||text.size()>131072)return fail(QStringLiteral("invalid_input"),QStringLiteral("Input string is empty or exceeds the request limit"));
  const auto *cancelFlag=cancel?cancel:&d->stopping;
  QString error;const auto vectors=d->interactive->embed(texts,cancelFlag,&error);
  if(vectors.size()!=texts.size())return fail(cancel&&cancel->load()?QStringLiteral("cancelled"):QStringLiteral("embedding_failed"),error.isEmpty()?QStringLiteral("Embedding response count mismatch"):error);
  QJsonArray data;
  for(qsizetype i=0;i<vectors.size();++i) {
    const auto &vector=vectors[i];if(vector.size()!=768)return fail(QStringLiteral("embedding_failed"),QStringLiteral("Embedding dimension mismatch"));
    QJsonArray row;double norm=0;
    for(float v:vector){if(!std::isfinite(v))return fail(QStringLiteral("embedding_failed"),QStringLiteral("Non-finite vector"));row.append(double(v));norm+=double(v)*v;}
    if(norm<=0)return fail(QStringLiteral("embedding_failed"),QStringLiteral("Zero vector"));
    data.append(QJsonObject{{"object","embedding"},{"index",double(i)},{"embedding",row}});
  }
  return {{"object","list"},{"model",d->config.modelId},{"data",data}};
}
void TaskService::stop() {
  if(d->stopping.exchange(true))return;
  {std::lock_guard lock(d->mutex);for(auto &pair:d->jobs)if(!terminal(pair.second->status))pair.second->cancelled=true;}
  d->available.notify_all();
  if(d->broker)d->broker->stop();
  if(d->queryThread.joinable())d->queryThread.join();if(d->indexThread.joinable())d->indexThread.join();
  std::lock_guard lock(d->mutex);for(auto &pair:d->jobs)if(!terminal(pair.second->status)){pair.second->status=QStringLiteral("cancelled");pair.second->updated=now();}d->persist();
}
}
