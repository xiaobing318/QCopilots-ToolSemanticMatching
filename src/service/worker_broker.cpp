// SPDX-License-Identifier: MIT
#include "worker_broker.h"
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QUuid>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

namespace qctsm {
class WorkerBroker::Impl {
public:
  struct Request {
    QString text;
    int priority=0;
    const std::atomic_bool *cancel=nullptr;
    QVector<float> vector;
    QString error;
    bool done=false;
    std::condition_variable changed;
  };
  ServiceConfig config;
  QString program;
  std::mutex mutex;
  std::condition_variable available;
  std::deque<std::shared_ptr<Request>> queue;
  std::atomic_bool stopping{false};
  std::thread thread;
  explicit Impl(const ServiceConfig &c):config(c) {
#ifdef Q_OS_WIN
    program=QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("qctsm-worker.exe"));
#else
    program=QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("qctsm-worker"));
#endif
    thread=std::thread([this]{run();});
  }
  void shutdown() {
    if(stopping.exchange(true)) return;
    available.notify_all();
    if(thread.joinable()) thread.join();
  }
  static void terminate(QProcess &p) {
    if(p.state()==QProcess::NotRunning) return;
    p.terminate();
    if(!p.waitForFinished(1500)) {p.kill(); p.waitForFinished(1500);}
  }
  bool launch(QProcess &process,QString &error) {
    if(process.state()!=QProcess::NotRunning) return true;
    if(!QFileInfo::exists(program)) {error=QStringLiteral("Packaged qctsm-worker is missing: ")+program;return false;}
    if(!QFileInfo::exists(config.modelPath)) {error=QStringLiteral("Model GGUF is missing. Download or import the model first.");return false;}
    QStringList args{QStringLiteral("--model"),config.modelPath,QStringLiteral("--backend"),config.backend,QStringLiteral("--threads"),QString::number(config.threads)};
    if(config.backend!=QStringLiteral("cpu") && !config.deviceId.isEmpty()) args<<QStringLiteral("--device-id")<<config.deviceId;
    process.setProgram(program); process.setArguments(args); process.setProcessChannelMode(QProcess::SeparateChannels);
    process.start();
    if(!process.waitForStarted(5000)) {error=process.errorString();return false;}
    return true;
  }
  void run() {
    QProcess process;
    while(!stopping.load()) {
      std::shared_ptr<Request> r;
      {
        std::unique_lock lock(mutex);
        available.wait(lock,[this]{return stopping.load()||!queue.empty();});
        if(stopping.load()) break;
        auto best=queue.begin();
        for(auto it=queue.begin();it!=queue.end();++it) if((*it)->priority>(*best)->priority) best=it;
        r=*best;queue.erase(best);
      }
      if(r->cancel && r->cancel->load()) r->error=QStringLiteral("cancelled");
      else if(launch(process,r->error)) {
        const QString id=QUuid::createUuid().toString(QUuid::WithoutBraces);
        const auto message=QJsonDocument(QJsonObject{{"id",id},{"operation","embed"},{"input",QJsonArray{r->text}}}).toJson(QJsonDocument::Compact)+'\n';
        if(process.write(message)!=message.size() || (process.bytesToWrite()>0 && !process.waitForBytesWritten(5000))) r->error=QStringLiteral("Cannot write to embedding worker");
        else {
          QElapsedTimer timer;timer.start();
          QByteArray buffer,diagnostics;
          bool replied=false;
          while(!replied) {
            if(stopping.load() || (r->cancel && r->cancel->load())) {r->error=QStringLiteral("cancelled");terminate(process);break;}
            if(timer.elapsed()>config.timeoutMs) {r->error=QStringLiteral("Embedding worker timed out");terminate(process);break;}
            process.waitForReadyRead(100);
            buffer+=process.readAllStandardOutput();diagnostics+=process.readAllStandardError();
            if(diagnostics.size()>4096) diagnostics=diagnostics.right(4096);
            if(buffer.size()>4*1024*1024) {r->error=QStringLiteral("Embedding worker response too large");terminate(process);break;}
            while(buffer.contains('\n')) {
              const auto at=buffer.indexOf('\n');const auto line=buffer.left(at);buffer.remove(0,at+1);
              const auto doc=QJsonDocument::fromJson(line);
              if(!doc.isObject()) {r->error=QStringLiteral("Invalid embedding worker JSON");terminate(process);replied=true;break;}
              const auto obj=doc.object();
              if(obj.value("id").toString()!=id) continue;
              replied=true;
              if(obj.value("status").toString()!=QStringLiteral("succeeded")) {r->error=obj.value("message").toString(obj.value("error_code").toString(QStringLiteral("Embedding worker failed")));break;}
              const auto rows=obj.value("data").toArray();
              if(rows.size()!=1 || !rows[0].isArray() || rows[0].toArray().size()!=768) {r->error=QStringLiteral("Invalid embedding worker vector dimensions");break;}
              double norm=0;
              for(const auto v:rows[0].toArray()) {
                if(!v.isDouble() || !std::isfinite(v.toDouble())) {r->error=QStringLiteral("Non-finite embedding worker vector");break;}
                r->vector.append(float(v.toDouble()));norm+=v.toDouble()*v.toDouble();
              }
              if(norm<=0) r->error=QStringLiteral("Zero embedding worker vector");
              if(!r->error.isEmpty()) r->vector.clear();
            }
            if(!replied && process.state()==QProcess::NotRunning) {r->error=QStringLiteral("Embedding worker exited: ")+QString::fromUtf8(diagnostics);break;}
          }
        }
      }
      {
        std::lock_guard lock(mutex);r->done=true;r->changed.notify_all();
      }
    }
    terminate(process);
    std::lock_guard lock(mutex);
    for(auto &r:queue) {r->error=QStringLiteral("Service is stopping");r->done=true;r->changed.notify_all();}
    queue.clear();
  }
};
WorkerBroker::WorkerBroker(const ServiceConfig &c):d(std::make_unique<Impl>(c)){}
WorkerBroker::~WorkerBroker(){stop();}
void WorkerBroker::stop(){d->shutdown();}
QList<QVector<float>> WorkerBroker::embed(const QStringList &texts,int priority,const std::atomic_bool *cancel,QString *error) {
  if(error) error->clear();
  QList<QVector<float>> vectors;
  for(const auto &text:texts) {
    auto r=std::make_shared<Impl::Request>();r->text=text;r->priority=priority;r->cancel=cancel;
    std::unique_lock lock(d->mutex);
    if(d->stopping.load()) {if(error)*error=QStringLiteral("Service is stopping");return {};}
    d->queue.push_back(r);d->available.notify_one();
    r->changed.wait(lock,[&r]{return r->done;});
    if(!r->error.isEmpty()) {if(error)*error=r->error;return {};}
    vectors.append(r->vector);
  }
  return vectors;
}
}
