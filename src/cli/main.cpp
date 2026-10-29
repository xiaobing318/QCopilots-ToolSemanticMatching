// SPDX-License-Identifier: MIT
#include "../service/service_config.h"

#include <QCoreApplication>
#include <QCommandLineParser>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTextStream>
#include <QTimer>
#include <QUrl>

#include <algorithm>
#include <limits>
#include <utility>

#ifndef QCTSM_APP_VERSION
#define QCTSM_APP_VERSION "0.1.0"
#endif

namespace {

constexpr qint64 MaxJsonBytes = 64 * 1024 * 1024;

QJsonObject failure(const QString &code, const QString &message)
{
  return {{QStringLiteral("status"), QStringLiteral("failed")},
          {QStringLiteral("error_code"), code},
          {QStringLiteral("message"), message}};
}

int printJson(const QJsonObject &value, int exitCode = 0)
{
  const QByteArray bytes = QJsonDocument(value).toJson(QJsonDocument::Indented);
  QFile output;
  if (!output.open(stdout, QIODevice::WriteOnly))
    return 1;
  if (output.write(bytes) != bytes.size() || !output.flush())
    return 1;
  return exitCode;
}

QString state(const QJsonObject &value)
{
  return value.value(QStringLiteral("status")).toString(
      value.value(QStringLiteral("state")).toString());
}

bool terminal(const QJsonObject &value)
{
  const QString current = state(value);
  return current == QStringLiteral("succeeded") || current == QStringLiteral("completed") ||
         current == QStringLiteral("failed") || current == QStringLiteral("cancelled") ||
         current == QStringLiteral("interrupted") || current == QStringLiteral("indeterminate");
}

int resultExitCode(const QJsonObject &value)
{
  const QString current = state(value);
  if (current == QStringLiteral("failed") || current == QStringLiteral("interrupted") ||
      current == QStringLiteral("indeterminate") || value.contains(QStringLiteral("error")))
    return 1;
  if (current == QStringLiteral("cancelled"))
    return 2;
  if (value.value(QStringLiteral("result")).isObject())
    return resultExitCode(value.value(QStringLiteral("result")).toObject());
  return 0;
}

QJsonDocument readJson(const QString &path, QString *error)
{
  QFile input(path);
  if (!input.open(QIODevice::ReadOnly)) {
    *error = QStringLiteral("Cannot read JSON file: %1").arg(input.errorString());
    return {};
  }
  if (input.size() > MaxJsonBytes) {
    *error = QStringLiteral("JSON input exceeds the 64 MiB limit.");
    return {};
  }
  QJsonParseError parseError;
  const QJsonDocument document = QJsonDocument::fromJson(input.readAll(), &parseError);
  if (parseError.error != QJsonParseError::NoError || document.isNull()) {
    *error = QStringLiteral("Invalid JSON at byte %1: %2")
                 .arg(parseError.offset).arg(parseError.errorString());
    return {};
  }
  return document;
}

QUrl serviceRoot(const QString &text, QString *error)
{
  QUrl url(text, QUrl::StrictMode);
  if (!url.isValid() || url.host().isEmpty() ||
      (url.scheme() != QStringLiteral("http") && url.scheme() != QStringLiteral("https")) ||
      !url.userInfo().isEmpty() || url.hasQuery() || url.hasFragment()) {
    *error = QStringLiteral("Endpoint must be an HTTP(S) service base URL without credentials, query or fragment.");
    return {};
  }
  QString path = url.path();
  if (!path.endsWith(QLatin1Char('/')))
    path += QLatin1Char('/');
  if (path.endsWith(QStringLiteral("/v1/")))
    path.chop(3);
  url.setPath(path);
  return url;
}

struct HttpResult {
  QJsonObject body;
  bool ok = false;
};

HttpResult request(QNetworkAccessManager &network, const QUrl &root,
                   const QByteArray &method, const QString &relativePath,
                   const QByteArray &body, const QString &token, int timeoutMs)
{
  QNetworkRequest request(root.resolved(QUrl(relativePath, QUrl::StrictMode)));
  request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
  request.setRawHeader("Accept", "application/json");
  // Service credentials must not follow a redirect to a different endpoint.
  request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                       QNetworkRequest::ManualRedirectPolicy);
  if (!token.isEmpty())
    request.setRawHeader("Authorization", QByteArrayLiteral("Bearer ") + token.toUtf8());

  QNetworkReply *reply = network.sendCustomRequest(request, method, body);
  QEventLoop eventLoop;
  QTimer timer;
  timer.setSingleShot(true);
  timer.setTimerType(Qt::PreciseTimer);
  bool timedOut = false;
  bool oversized = false;
  QByteArray received;
  QObject::connect(reply, &QNetworkReply::readyRead, &eventLoop, [&] {
    received += reply->readAll();
    if (received.size() > MaxJsonBytes) {
      oversized = true;
      reply->abort();
    }
  });
  QObject::connect(reply, &QNetworkReply::finished, &eventLoop, &QEventLoop::quit);
  QObject::connect(&timer, &QTimer::timeout, &eventLoop, [&] {
    timedOut = true;
    reply->abort();
    eventLoop.quit();
  });
  timer.start(std::max(1, timeoutMs));
  if (!reply->isFinished())
    eventLoop.exec();
  timer.stop();
  received += reply->readAll();
  const int statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
  const QNetworkReply::NetworkError networkError = reply->error();
  const QString networkMessage = reply->errorString();
  delete reply;

  if (timedOut)
    return {failure(QStringLiteral("request_timeout"), QStringLiteral("The HTTP request timed out. A submitted job may still be running.")), false};
  if (oversized || received.size() > MaxJsonBytes)
    return {failure(QStringLiteral("response_too_large"), QStringLiteral("The service response exceeds the 64 MiB limit.")), false};

  QJsonParseError parseError;
  const QJsonDocument document = QJsonDocument::fromJson(received, &parseError);
  QJsonObject result = document.isObject() ? document.object() : QJsonObject{};
  if (statusCode >= 300 && statusCode < 400)
    return {failure(QStringLiteral("redirect_rejected"), QStringLiteral("The service redirected this request. Configure its final base URL explicitly.")), false};
  if (statusCode < 200 || statusCode >= 300 || networkError != QNetworkReply::NoError) {
    if (result.isEmpty())
      result = failure(QStringLiteral("http_error"), statusCode > 0
                         ? QStringLiteral("The service returned HTTP %1.").arg(statusCode)
                         : networkMessage);
    result.insert(QStringLiteral("http_status"), statusCode);
    return {result, false};
  }
  if (parseError.error != QJsonParseError::NoError || !document.isObject())
    return {failure(QStringLiteral("invalid_response"), QStringLiteral("The service did not return a JSON object.")), false};
  return {result, true};
}

QString segment(const QString &value)
{
  return QString::fromLatin1(QUrl::toPercentEncoding(value));
}

bool positiveInteger(const QCommandLineParser &parser, const QString &option,
                     int minimum, int maximum, int *value)
{
  bool ok = false;
  const qlonglong parsed = parser.value(option).toLongLong(&ok);
  if (!ok || parsed < minimum || parsed > maximum)
    return false;
  *value = static_cast<int>(parsed);
  return true;
}

} // namespace

int main(int argc, char **argv)
{
  QCoreApplication application(argc, argv);
  QCoreApplication::setApplicationName(QStringLiteral("qctsm-cli"));
  QCoreApplication::setApplicationVersion(QStringLiteral(QCTSM_APP_VERSION));
  QCommandLineParser parser;
  parser.setApplicationDescription(QStringLiteral(
      "QCopilots Tool Semantic Matching JSON command-line client.\n"
      "Commands: health, models, config, search, import, records, job, cancel,\n"
      "          embeddings, model-info, model-download, model-import, shutdown.\n"
      "All operations output JSON. Start qctsm-service before remote commands.\n"
      "A --wait timeout stops waiting only and never implicitly cancels a job."));
  parser.addHelpOption();
  parser.addVersionOption();
  parser.addPositionalArgument(QStringLiteral("command"), QStringLiteral("Operation to perform."));
  parser.addPositionalArgument(QStringLiteral("arguments"), QStringLiteral("Optional query, record IDs or job ID."), QStringLiteral("[arguments...]"));
  parser.addOptions({
      {QStringLiteral("profile"), QStringLiteral("Local profile directory."), QStringLiteral("directory")},
      {QStringLiteral("endpoint"), QStringLiteral("Service root URL, overrides the profile endpoint."), QStringLiteral("url")},
      {QStringLiteral("token"), QStringLiteral("Service bearer token, overrides the profile token."), QStringLiteral("token")},
      {QStringLiteral("query"), QStringLiteral("Natural-language query."), QStringLiteral("text")},
      {QStringLiteral("top-n"), QStringLiteral("Maximum candidate count (1-100)."), QStringLiteral("number"), QStringLiteral("8")},
      {QStringLiteral("source"), QStringLiteral("Source ID for import or search filtering."), QStringLiteral("id")},
      {QStringLiteral("kind"), QStringLiteral("Search record kind: definition or example."), QStringLiteral("kind")},
      {QStringLiteral("available-only"), QStringLiteral("Search only available capabilities.")},
      {QStringLiteral("file"), QStringLiteral("JSON input file or model GGUF file, depending on command."), QStringLiteral("path")},
      {QStringLiteral("ids"), QStringLiteral("Comma-separated record IDs."), QStringLiteral("ids")},
      {QStringLiteral("job-id"), QStringLiteral("Job ID for job or cancel."), QStringLiteral("id")},
      {QStringLiteral("input"), QStringLiteral("Raw embedding text, without automatic retrieval prefixes."), QStringLiteral("text")},
      {QStringLiteral("model"), QStringLiteral("Embedding model ID."), QStringLiteral("id")},
      {QStringLiteral("wait"), QStringLiteral("Wait for a returned job to reach a terminal state.")},
      {QStringLiteral("wait-timeout"), QStringLiteral("Maximum total wait in seconds (1-86400)."), QStringLiteral("seconds"), QStringLiteral("600")},
      {QStringLiteral("timeout"), QStringLiteral("HTTP request timeout in seconds (1-3600)."), QStringLiteral("seconds"), QStringLiteral("120")}
  });
  if (!parser.parse(application.arguments()))
    return printJson(failure(QStringLiteral("invalid_arguments"), parser.errorText()), 1);
  if (parser.isSet(QStringLiteral("help"))) {
    QTextStream(stdout) << parser.helpText();
    return 0;
  }
  if (parser.isSet(QStringLiteral("version"))) {
    QTextStream(stdout) << application.applicationName() << ' ' << application.applicationVersion() << '\n';
    return 0;
  }
  QStringList positional = parser.positionalArguments();
  if (positional.isEmpty()) {
    QTextStream(stdout) << parser.helpText();
    return 0;
  }
  const QString command = positional.takeFirst();
  QString error;
  const qctsm::ServiceConfig config = qctsm::ServiceConfig::load(parser.value(QStringLiteral("profile")), &error);
  if (!error.isEmpty())
    return printJson(failure(QStringLiteral("config_error"), error), 1);
  if (command == QStringLiteral("config"))
    return printJson(config.toJson(false));

  int timeoutSeconds = 0;
  int waitSeconds = 0;
  if (!positiveInteger(parser, QStringLiteral("timeout"), 1, 3600, &timeoutSeconds) ||
      !positiveInteger(parser, QStringLiteral("wait-timeout"), 1, 86400, &waitSeconds))
    return printJson(failure(QStringLiteral("invalid_arguments"), QStringLiteral("Invalid HTTP timeout or wait timeout.")), 1);
  QUrl configured;
  configured.setScheme(QStringLiteral("http"));
  configured.setHost(config.host);
  configured.setPort(config.port);
  const QUrl root = serviceRoot(parser.isSet(QStringLiteral("endpoint"))
                                  ? parser.value(QStringLiteral("endpoint")) : configured.toString(), &error);
  if (!error.isEmpty())
    return printJson(failure(QStringLiteral("invalid_endpoint"), error), 1);
  const QString token = parser.isSet(QStringLiteral("token")) ? parser.value(QStringLiteral("token")) : config.token;
  QByteArray method = QByteArrayLiteral("GET");
  QString path;
  QJsonDocument payload;

  if (command == QStringLiteral("health"))
    path = QStringLiteral("health");
  else if (command == QStringLiteral("models"))
    path = QStringLiteral("v1/models");
  else if (command == QStringLiteral("model-info"))
    path = QStringLiteral("v1/model/info");
  else if (command == QStringLiteral("model-download") || command == QStringLiteral("shutdown")) {
    method = QByteArrayLiteral("POST");
    path = command == QStringLiteral("shutdown") ? QStringLiteral("v1/shutdown") : QStringLiteral("v1/model/download");
    payload = QJsonDocument(QJsonObject{});
  } else if (command == QStringLiteral("model-import")) {
    if (parser.value(QStringLiteral("file")).isEmpty())
      return printJson(failure(QStringLiteral("invalid_arguments"), QStringLiteral("model-import requires --file <GGUF path>.")), 1);
    method = QByteArrayLiteral("POST");
    path = QStringLiteral("v1/model/import");
    payload = QJsonDocument(QJsonObject{{QStringLiteral("path"), parser.value(QStringLiteral("file"))}});
  } else if (command == QStringLiteral("search")) {
    const QString query = parser.isSet(QStringLiteral("query")) ? parser.value(QStringLiteral("query")) : positional.join(QLatin1Char(' '));
    int topN = 0;
    if (query.trimmed().isEmpty() || !positiveInteger(parser, QStringLiteral("top-n"), 1, 100, &topN))
      return printJson(failure(QStringLiteral("invalid_arguments"), QStringLiteral("search requires a nonempty query and --top-n from 1 to 100.")), 1);
    QJsonObject filters;
    if (parser.isSet(QStringLiteral("source")))
      filters.insert(QStringLiteral("source_id"), parser.value(QStringLiteral("source")));
    if (parser.isSet(QStringLiteral("kind"))) {
      const QString kind = parser.value(QStringLiteral("kind"));
      if (kind != QStringLiteral("definition") && kind != QStringLiteral("example"))
        return printJson(failure(QStringLiteral("invalid_arguments"), QStringLiteral("--kind must be definition or example.")), 1);
      filters.insert(QStringLiteral("kind"), kind);
    }
    if (parser.isSet(QStringLiteral("available-only")))
      filters.insert(QStringLiteral("available"), true);
    payload = QJsonDocument(QJsonObject{{QStringLiteral("query"), query}, {QStringLiteral("top_n"), topN}, {QStringLiteral("filters"), filters}});
    method = QByteArrayLiteral("POST");
    path = QStringLiteral("v1/search");
  } else if (command == QStringLiteral("import")) {
    if (parser.value(QStringLiteral("file")).isEmpty())
      return printJson(failure(QStringLiteral("invalid_arguments"), QStringLiteral("import requires --file <JSON path>.")), 1);
    payload = readJson(parser.value(QStringLiteral("file")), &error);
    if (!error.isEmpty())
      return printJson(failure(QStringLiteral("invalid_json"), error), 1);
    QString source = parser.value(QStringLiteral("source"));
    if (source.isEmpty() && payload.isObject())
      source = payload.object().value(QStringLiteral("source_id")).toString();
    if (source.isEmpty())
      return printJson(failure(QStringLiteral("invalid_arguments"), QStringLiteral("import requires --source or a source_id in the catalog JSON.")), 1);
    path = QStringLiteral("v1/catalogs/") + segment(source);
    method = QByteArrayLiteral("PUT");
  } else if (command == QStringLiteral("records")) {
    QStringList ids = parser.value(QStringLiteral("ids")).split(QLatin1Char(','), Qt::SkipEmptyParts);
    ids.append(positional);
    if (ids.isEmpty())
      return printJson(failure(QStringLiteral("invalid_arguments"), QStringLiteral("records requires --ids or positional record IDs.")), 1);
    QJsonArray values;
    for (const QString &id : ids)
      values.append(id);
    payload = QJsonDocument(QJsonObject{{QStringLiteral("ids"), values}});
    method = QByteArrayLiteral("POST");
    path = QStringLiteral("v1/records/get");
  } else if (command == QStringLiteral("job") || command == QStringLiteral("cancel")) {
    const QString id = parser.isSet(QStringLiteral("job-id")) ? parser.value(QStringLiteral("job-id")) : positional.value(0);
    if (id.isEmpty())
      return printJson(failure(QStringLiteral("invalid_arguments"), QStringLiteral("job and cancel require --job-id or a positional job ID.")), 1);
    path = QStringLiteral("v1/jobs/") + segment(id);
    if (command == QStringLiteral("cancel")) {
      path += QStringLiteral("/cancel");
      method = QByteArrayLiteral("POST");
      payload = QJsonDocument(QJsonObject{});
    }
  } else if (command == QStringLiteral("embeddings")) {
    if (parser.isSet(QStringLiteral("input")) == parser.isSet(QStringLiteral("file")))
      return printJson(failure(QStringLiteral("invalid_arguments"), QStringLiteral("embeddings requires exactly one of --input or --file (a JSON string array).")), 1);
    QJsonValue input;
    if (parser.isSet(QStringLiteral("input")))
      input = parser.value(QStringLiteral("input"));
    else {
      const QJsonDocument inputDocument = readJson(parser.value(QStringLiteral("file")), &error);
      if (!error.isEmpty() || !inputDocument.isArray() || inputDocument.array().isEmpty())
        return printJson(failure(QStringLiteral("invalid_json"), error.isEmpty() ? QStringLiteral("Embedding input file must contain a nonempty JSON array of strings.") : error), 1);
      for (const QJsonValue &item : inputDocument.array()) {
        if (!item.isString())
          return printJson(failure(QStringLiteral("invalid_json"), QStringLiteral("All embedding inputs must be strings.")), 1);
      }
      input = inputDocument.array();
    }
    payload = QJsonDocument(QJsonObject{{QStringLiteral("model"), parser.isSet(QStringLiteral("model")) ? parser.value(QStringLiteral("model")) : config.modelId},
                                       {QStringLiteral("input"), input}, {QStringLiteral("encoding_format"), QStringLiteral("float")}});
    method = QByteArrayLiteral("POST");
    path = QStringLiteral("v1/embeddings");
  } else {
    return printJson(failure(QStringLiteral("unknown_command"), QStringLiteral("Unknown command: %1. Use --help for available commands.").arg(command)), 1);
  }

  QNetworkAccessManager network;
  const QByteArray bytes = payload.isNull() ? QByteArray{} : payload.toJson(QJsonDocument::Compact);
  HttpResult response = request(network, root, method, path, bytes, token, timeoutSeconds * 1000);
  if (!response.ok)
    return printJson(response.body, 1);
  if (!parser.isSet(QStringLiteral("wait")) || terminal(response.body))
    return printJson(response.body, resultExitCode(response.body));
  const QString jobId = response.body.value(QStringLiteral("job_id")).toString(
      response.body.value(QStringLiteral("id")).toString());
  if (jobId.isEmpty())
    return printJson(response.body, resultExitCode(response.body));

  QElapsedTimer elapsed;
  elapsed.start();
  const qint64 limitMs = static_cast<qint64>(waitSeconds) * 1000;
  while (elapsed.elapsed() < limitMs) {
    const qint64 remaining = limitMs - elapsed.elapsed();
    if (remaining <= 0)
      break;
    const bool limitedByWait = remaining <= static_cast<qint64>(timeoutSeconds) * 1000;
    HttpResult polled = request(network, root, QByteArrayLiteral("GET"), QStringLiteral("v1/jobs/") + segment(jobId), {}, token,
                               static_cast<int>(std::min<qint64>(timeoutSeconds * 1000, remaining)));
    if (!polled.ok) {
      // A request truncated by the total wait budget is a wait timeout, not a
      // service failure. Preserve the last successful job snapshot for callers.
      if (limitedByWait && polled.body.value(QStringLiteral("error_code")) == QStringLiteral("request_timeout"))
        break;
      return printJson(polled.body, 1);
    }
    response = std::move(polled);
    if (terminal(response.body))
      return printJson(response.body, resultExitCode(response.body));
    QEventLoop pause;
    QTimer::singleShot(static_cast<int>(std::min<qint64>(500, std::max<qint64>(0, limitMs - elapsed.elapsed()))), &pause, &QEventLoop::quit);
    pause.exec();
  }
  QJsonObject timeout = failure(QStringLiteral("wait_timeout"), QStringLiteral("Stopped waiting. The job was not cancelled and may still be running."));
  timeout.insert(QStringLiteral("job_id"), jobId);
  timeout.insert(QStringLiteral("last_snapshot"), response.body);
  return printJson(timeout, 1);
}
