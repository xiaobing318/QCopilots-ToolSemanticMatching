// SPDX-License-Identifier: MIT
#include "http_server.h"
#include "task_service.h"
#include <httplib.h>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QUrl>
#include <atomic>
#include <chrono>
#include <thread>

namespace qctsm {
namespace {
void reply(httplib::Response &res,const QJsonObject &obj,int code=200) {
  res.status=code;
  const auto data=QJsonDocument(obj).toJson(QJsonDocument::Compact);
  res.set_content(std::string(data.constData(),size_t(data.size())),"application/json; charset=utf-8");
  res.set_header("Cache-Control","no-store");
  res.set_header("X-Content-Type-Options","nosniff");
}
void error(httplib::Response &res,const char *code,const QString &message,int status=400) {
  reply(res,{{"status","failed"},{"error_code",QLatin1String(code)},{"message",message},{"error",QJsonObject{{"type",QLatin1String(code)},{"message",message}}}},status);
}
QJsonDocument parse(const httplib::Request &req,httplib::Response &res) {
  QJsonParseError e;const auto doc=QJsonDocument::fromJson(QByteArray(req.body.data(),qsizetype(req.body.size())),&e);
  if(e.error!=QJsonParseError::NoError || doc.isNull())error(res,"invalid_json",e.errorString());
  return doc;
}
bool objectBody(const httplib::Request &req,httplib::Response &res,QJsonObject &object) {
  const auto doc=parse(req,res);if(doc.isNull())return false;
  if(!doc.isObject()){error(res,"invalid_json",QStringLiteral("Expected a JSON object"));return false;}
  object=doc.object();return true;
}
}
class HttpServer::Impl {
public:
  ServiceConfig config;
  std::shared_ptr<TaskService> service;
  httplib::Server server;
  std::thread thread;
  std::atomic_bool shutdown{false};
  int boundPort=0;
  Impl(const ServiceConfig &c,std::shared_ptr<TaskService> s):config(c),service(std::move(s)) {
    server.set_payload_max_length(16*1024*1024);
    server.set_read_timeout(10,0);
    server.set_write_timeout(30,0);
    server.set_keep_alive_max_count(100);
    server.set_pre_routing_handler([this](const httplib::Request &req,httplib::Response &res){
      // Do not allow webpages to turn this loopback API into a cross-origin proxy.
      if(req.has_header("Origin")) {error(res,"origin_not_allowed",QStringLiteral("Use a native HTTP client"),403);return httplib::Server::HandlerResponse::Handled;}
      if(req.path=="/health")return httplib::Server::HandlerResponse::Unhandled;
      if(!config.token.isEmpty()) {
        const auto expected=(QStringLiteral("Bearer ")+config.token).toUtf8();
        const auto header=req.get_header_value("Authorization");
        unsigned mismatch=unsigned(header.size()!=size_t(expected.size()));
        const auto count=std::max(header.size(),size_t(expected.size()));
        for(size_t i=0;i<count;++i)mismatch|=unsigned((i<header.size()?static_cast<unsigned char>(header[i]):0)^(i<size_t(expected.size())?static_cast<unsigned char>(expected.at(qsizetype(i))):0));
        if(mismatch){error(res,"unauthorized",QStringLiteral("A valid service bearer token is required"),401);return httplib::Server::HandlerResponse::Handled;}
      }
      return httplib::Server::HandlerResponse::Unhandled;
    });
    server.set_exception_handler([](const httplib::Request &,httplib::Response &res,std::exception_ptr exception){
      QString message=QStringLiteral("Internal service error");
      try{if(exception)std::rethrow_exception(exception);}catch(const std::exception &e){message=QString::fromUtf8(e.what());}catch(...){}
      error(res,"internal_error",message,500);
    });
    server.Get("/health",[this](const httplib::Request &,httplib::Response &res){reply(res,{{"status","ok"},{"service","qctsm"},{"version","0.1.0"},{"model",config.modelId},{"provider",config.provider}});});
    server.Get("/v1/models",[this](const httplib::Request &,httplib::Response &res){reply(res,{{"object","list"},{"data",QJsonArray{QJsonObject{{"id",config.modelId},{"object","model"},{"owned_by","qctsm"},{"capabilities",QJsonArray{"embeddings"}},{"revision",config.modelRevision}}}}});});
    server.Post("/v1/embeddings",[this](const httplib::Request &req,httplib::Response &res){
      QJsonObject body;if(!objectBody(req,res,body))return;
      std::atomic_bool cancelled{false};
      std::jthread watcher([this,&req,&cancelled](std::stop_token stop){
        while(!stop.stop_requested()) {
          if(shutdown.load()||req.is_connection_closed()){cancelled=true;return;}
          std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
      });
      auto result=service->embeddings(body,&cancelled);
      watcher.request_stop();watcher.join();
      if(result.value("status").toString()==QStringLiteral("failed")) {const auto code=result.value("error_code").toString();error(res,code.toUtf8().constData(),result.value("message").toString(),code==QStringLiteral("embedding_failed")?503:400);return;}
      reply(res,result);
    });
    server.Put(R"(/v1/catalogs/([^/]+))",[this](const httplib::Request &req,httplib::Response &res){
      const auto doc=parse(req,res);if(doc.isNull())return;
      const auto id=QUrl::fromPercentEncoding(QByteArray::fromStdString(req.matches[1].str()));
      if(id.isEmpty()||id.size()>256){error(res,"invalid_source_id",QStringLiteral("source_id must contain 1 to 256 characters"));return;}
      const auto catalog=normalizeCatalog(doc.isArray()?QJsonValue(doc.array()):QJsonValue(doc.object()),id);
      if(catalog.value("status").toString()==QStringLiteral("failed")){reply(res,catalog,400);return;}
      auto result=service->submit(QStringLiteral("sync_catalog"),catalog);reply(res,result,result.value("status").toString()==QStringLiteral("queued")?202:503);
    });
    server.Post("/v1/search",[this](const httplib::Request &req,httplib::Response &res){
      QJsonObject body;if(!objectBody(req,res,body))return;
      if(!body.value("query").isString()||body.value("query").toString().trimmed().isEmpty()){error(res,"invalid_query",QStringLiteral("query must be a non-empty string"));return;}
      auto result=service->submit(QStringLiteral("search"),body);reply(res,result,result.value("status").toString()==QStringLiteral("queued")?202:503);
    });
    server.Post("/v1/records/get",[this](const httplib::Request &req,httplib::Response &res){
      QJsonObject body;if(!objectBody(req,res,body))return;
      const auto values=body.value("ids");if(!values.isArray()||values.toArray().isEmpty()||values.toArray().size()>100){error(res,"invalid_ids",QStringLiteral("ids must contain 1 to 100 IDs"));return;}
      QStringList ids;for(const auto &v:values.toArray()){if(!v.isString()||v.toString().isEmpty()){error(res,"invalid_ids",QStringLiteral("Every ID must be a non-empty string"));return;}ids<<v.toString();}
      auto result=service->getRecords(ids);reply(res,result,result.value("status").toString()==QStringLiteral("failed")?400:200);
    });
    server.Get(R"(/v1/jobs/([^/]+))",[this](const httplib::Request &req,httplib::Response &res){auto result=service->snapshot(QString::fromStdString(req.matches[1].str()));reply(res,result,result.value("error_code").toString()==QStringLiteral("job_not_found")?404:200);});
    server.Post(R"(/v1/jobs/([^/]+)/cancel)",[this](const httplib::Request &req,httplib::Response &res){auto result=service->cancel(QString::fromStdString(req.matches[1].str()));reply(res,result,result.value("error_code").toString()==QStringLiteral("job_not_found")?404:200);});
    server.Get("/v1/index/status",[this](const httplib::Request &,httplib::Response &res){reply(res,service->indexStatus());});
    server.Get("/v1/model/info",[this](const httplib::Request &,httplib::Response &res){reply(res,service->modelInfo());});
    server.Post("/v1/model/download",[this](const httplib::Request &,httplib::Response &res){auto result=service->submit(QStringLiteral("model_download"),{});reply(res,result,result.value("status").toString()==QStringLiteral("queued")?202:503);});
    server.Post("/v1/model/import",[this](const httplib::Request &req,httplib::Response &res){QJsonObject body;if(!objectBody(req,res,body))return;if(body.value("path").toString().isEmpty()){error(res,"invalid_path",QStringLiteral("path is required"));return;}auto result=service->submit(QStringLiteral("model_import"),body);reply(res,result,result.value("status").toString()==QStringLiteral("queued")?202:503);});
    server.Post("/v1/shutdown",[this](const httplib::Request &,httplib::Response &res){reply(res,{{"status","succeeded"},{"message","Service shutdown requested"}});shutdown=true;});
    server.set_error_handler([](const httplib::Request &,httplib::Response &res){if(res.body.empty())error(res,"http_error",QStringLiteral("HTTP request failed"),res.status);});
  }
};
HttpServer::HttpServer(const ServiceConfig &c,std::shared_ptr<TaskService> s):d(std::make_unique<Impl>(c,std::move(s))){}
HttpServer::~HttpServer(){stop();}
bool HttpServer::start(QString *error) {
  if(error)error->clear();
  if(d->thread.joinable()){if(error)*error=QStringLiteral("Server already started");return false;}
  const auto host=d->config.host.toStdString();
  if(d->config.port==0)d->boundPort=d->server.bind_to_any_port(host);
  else if(d->server.bind_to_port(host,d->config.port))d->boundPort=d->config.port;
  if(d->boundPort<=0){if(error)*error=QStringLiteral("Cannot bind local service address. Check whether the port is already in use.");return false;}
  d->thread=std::thread([this]{d->server.listen_after_bind();});return true;
}
void HttpServer::stop(){d->shutdown=true;d->server.stop();if(d->thread.joinable())d->thread.join();}
int HttpServer::port()const{return d->boundPort;}
bool HttpServer::shutdownRequested()const{return d->shutdown.load();}
}
