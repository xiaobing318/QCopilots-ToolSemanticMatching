// SPDX-License-Identifier: MIT
#include "http_server.h"
#include "service_config.h"
#include "task_service.h"
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QLockFile>
#include <QMutex>
#include <QMutexLocker>
#include <QTextStream>
#include <QTimer>
#include <csignal>

namespace {
volatile std::sig_atomic_t interrupted=0;
void interrupt(int){interrupted=1;}
QFile *logFile=nullptr;
QMutex logMutex;
void logMessage(QtMsgType type,const QMessageLogContext &,const QString &text){
  QMutexLocker guard(&logMutex);
  if(logFile){const auto line=QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)+QStringLiteral(" ")+QString::number(type)+QStringLiteral(" ")+text+'\n';logFile->write(line.toUtf8());logFile->flush();}
}
}
int main(int argc,char **argv) {
  QCoreApplication app(argc,argv);app.setApplicationName(QStringLiteral("qctsm-service"));app.setApplicationVersion(QStringLiteral("0.1.0"));app.setOrganizationName(QStringLiteral("QCopilots"));
  QCommandLineParser parser;parser.setApplicationDescription(QStringLiteral("QCopilots Tool Semantic Matching local HTTP service"));parser.addHelpOption();parser.addVersionOption();
  parser.addOptions({{{"p","profile"},QStringLiteral("Profile directory (config.json and service log)."),QStringLiteral("path")},{"port",QStringLiteral("Override loopback port, 0 selects a free port."),"port"},{"smoke-test",QStringLiteral("Start and stop the HTTP service without loading a model.")}});
  parser.process(app);
  QString error;auto config=qctsm::ServiceConfig::load(parser.value("profile"),&error);
  if(!error.isEmpty()){QTextStream(stderr)<<error<<Qt::endl;return 2;}
  if(!QDir().mkpath(config.profilePath)){QTextStream(stderr)<<"Cannot create service profile"<<Qt::endl;return 2;}
  QLockFile instance(QDir(config.profilePath).filePath(QStringLiteral("service.lock")));instance.setStaleLockTime(0);
  if(!instance.tryLock()){QTextStream(stderr)<<"A service already owns this profile"<<Qt::endl;return 3;}
  // Another first-time starter may have created config.json while we waited.
  config=qctsm::ServiceConfig::load(config.profilePath,&error);
  if(!error.isEmpty()){QTextStream(stderr)<<error<<Qt::endl;return 2;}
  if(!QFileInfo::exists(config.configFilePath())&&!config.save(&error)){QTextStream(stderr)<<error<<Qt::endl;return 2;}
  if(parser.isSet("port")){bool ok=false;const int port=parser.value("port").toInt(&ok);if(!ok||port<0||port>65535){QTextStream(stderr)<<"Invalid --port"<<Qt::endl;return 2;}config.port=port;}
  if(parser.isSet("smoke-test"))config.port=0;
  QFile file(QDir(config.profilePath).filePath(QStringLiteral("service.log")));
  if(file.size()>2*1024*1024){file.remove();}
  if(file.open(QIODevice::WriteOnly|QIODevice::Append)){logFile=&file;qInstallMessageHandler(logMessage);}
  int code=0;
  {
    auto service=std::make_shared<qctsm::TaskService>(config);qctsm::HttpServer server(config,service);
    if(!server.start(&error)){QTextStream(stderr)<<error<<Qt::endl;code=3;}
    else {
      QTextStream(stdout)<<QJsonDocument(QJsonObject{{"status","ready"},{"host",config.host},{"port",server.port()},{"version","0.1.0"}}).toJson(QJsonDocument::Compact)<<Qt::endl;
      std::signal(SIGINT,interrupt);std::signal(SIGTERM,interrupt);
      QTimer timer;timer.setInterval(100);QObject::connect(&timer,&QTimer::timeout,&app,[&]{if(interrupted||server.shutdownRequested())app.quit();});timer.start();
      if(parser.isSet("smoke-test"))QTimer::singleShot(300,&app,&QCoreApplication::quit);
      code=app.exec();
    }
    // Stop accepting requests and cancel connected embedding calls before joining jobs.
    server.stop();service->stop();
  }
  qInstallMessageHandler(nullptr);logFile=nullptr;file.close();return code;
}
