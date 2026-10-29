// SPDX-License-Identifier: MIT
#include "main_window.h"
#include "service/service_config.h"
#include <QApplication>
#include <QCommandLineParser>
#include <QTextStream>
#include <QTimer>

#ifndef QCTSM_APP_VERSION
#define QCTSM_APP_VERSION "0.1.0"
#endif

int main(int argc, char **argv)
{
  // Help/version must also work on headless package-verification hosts.
  for (int i = 1; i < argc; ++i) {
    const QByteArray argument(argv[i]);
    if (argument == "--version" || argument == "-v") {
      QTextStream(stdout) << "qctsm-gui " << QCTSM_APP_VERSION << Qt::endl;
      return 0;
    }
    if (argument == "--help" || argument == "-h") {
      QTextStream(stdout) << "QCopilots Tool Semantic Matching\n"
        "Usage: qctsm-gui [--profile <directory>] [--smoke-test]\n"
        "  --profile <directory>  Isolated configuration and data profile\n"
        "  --smoke-test           Show the window briefly without starting a service\n"
        "  --version             Print application version\n";
      return 0;
    }
  }
  QApplication app(argc, argv);
  QCoreApplication::setApplicationName(QStringLiteral("qctsm-gui"));
  QCoreApplication::setApplicationVersion(QStringLiteral(QCTSM_APP_VERSION));
  QCoreApplication::setOrganizationName(QStringLiteral("QCopilots"));
  QCommandLineParser parser;
  parser.setApplicationDescription(QStringLiteral("QCopilots 工具语义匹配"));
  parser.addHelpOption();
  parser.addVersionOption();
  const QCommandLineOption profile(QStringLiteral("profile"), QStringLiteral("配置与数据目录"), QStringLiteral("directory"));
  const QCommandLineOption smoke(QStringLiteral("smoke-test"), QStringLiteral("显示窗口后退出，不启动服务"));
  parser.addOption(profile);
  parser.addOption(smoke);
  parser.process(app);
  QString error;
  const auto config = qctsm::ServiceConfig::load(parser.value(profile), &error);
  qctsm::MainWindow window(config, parser.isSet(smoke));
  window.show();
  if (!error.isEmpty()) window.reportStartupError(error);
  if (parser.isSet(smoke)) QTimer::singleShot(250, &app, &QCoreApplication::quit);
  return app.exec();
}

