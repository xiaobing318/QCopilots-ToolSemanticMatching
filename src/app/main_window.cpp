// SPDX-License-Identifier: MIT
#include "main_window.h"
#include "service/task_service.h"
#include "runtime/model_files.h"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPlainTextEdit>
#include <QProcess>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QSplitter>
#include <QStackedWidget>
#include <QStatusBar>
#include <QTableWidget>
#include <QTimer>
#include <QUrl>
#include <QUuid>
#include <QVBoxLayout>
#include <algorithm>

namespace {
QLabel *textLabel(const QString &text, QWidget *parent = nullptr)
{
  auto *label = new QLabel(text, parent);
  label->setTextFormat(Qt::PlainText);
  label->setWordWrap(true);
  return label;
}

QLabel *heading(const QString &text, QWidget *parent = nullptr)
{
  auto *label = textLabel(text, parent);
  auto font = label->font();
  font.setPointSize(font.pointSize() + 5);
  font.setBold(true);
  label->setFont(font);
  return label;
}

QWidget *fieldWithButton(QLineEdit *field, const QString &caption, std::function<void()> action)
{
  auto *row = new QWidget;
  auto *layout = new QHBoxLayout(row);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->addWidget(field, 1);
  auto *button = new QPushButton(caption, row);
  layout->addWidget(button);
  QObject::connect(button, &QPushButton::clicked, row, [action = std::move(action)] { action(); });
  return row;
}

QString jsonText(const QJsonObject &object)
{
  return QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Indented));
}

QString stateText(const QString &state)
{
  if (state == QLatin1String("queued")) return QStringLiteral("排队中");
  if (state == QLatin1String("running")) return QStringLiteral("执行中");
  if (state == QLatin1String("succeeded") || state == QLatin1String("completed")) return QStringLiteral("已完成");
  if (state == QLatin1String("cancelled")) return QStringLiteral("已取消");
  if (state == QLatin1String("cancelling")) return QStringLiteral("正在取消");
  if (state == QLatin1String("failed")) return QStringLiteral("失败");
  return state;
}

bool terminal(const QString &status)
{
  return status == QLatin1String("succeeded") || status == QLatin1String("completed")
    || status == QLatin1String("failed") || status == QLatin1String("cancelled");
}

QString errorText(const QJsonObject &object)
{
  QString message = object.value(QStringLiteral("message")).toString();
  if (message.isEmpty()) message = object.value(QStringLiteral("error")).toObject().value(QStringLiteral("message")).toString();
  if (message.isEmpty()) message = object.value(QStringLiteral("error")).toString();
  const auto code = object.value(QStringLiteral("error_code")).toString();
  return !code.isEmpty() ? code + QStringLiteral(": ") + message : message;
}

}

namespace qctsm {
MainWindow::MainWindow(const ServiceConfig &config, bool smokeTest, QWidget *parent)
  : QMainWindow(parent), m_config(config), m_connectedConfig(config), m_smokeTest(smokeTest)
{
  setObjectName(QStringLiteral("QctsmMainWindow"));
  setWindowTitle(QStringLiteral("QCopilots Tool Semantic Matching"));
  setMinimumSize(920, 650);
  resize(1180, 800);
  m_network = new QNetworkAccessManager(this);
  auto *central = new QWidget(this);
  auto *outer = new QVBoxLayout(central);
  outer->setContentsMargins(16, 16, 16, 10);
  auto *top = new QHBoxLayout;
  top->addWidget(heading(QStringLiteral("QCopilots 工具语义匹配")), 1);
  m_connection = textLabel(QStringLiteral("服务尚未连接"));
  top->addWidget(m_connection);
  auto *connectionButton = new QPushButton(QStringLiteral("检查连接"));
  connectionButton->setObjectName(QStringLiteral("checkConnection"));
  top->addWidget(connectionButton);
  connect(connectionButton, &QPushButton::clicked, this, &MainWindow::checkService);
  outer->addLayout(top);
  m_banner = textLabel(QStringLiteral("输入需求，从工具定义与调用示例中查找相关能力。"));
  m_banner->setObjectName(QStringLiteral("notification"));
  m_banner->setMinimumHeight(38);
  outer->addWidget(m_banner);
  auto *body = new QHBoxLayout;
  m_navigation = new QListWidget;
  m_navigation->setObjectName(QStringLiteral("navigation"));
  m_navigation->addItems({QStringLiteral("工具检索"), QStringLiteral("目录与索引"), QStringLiteral("模型与服务")});
  m_navigation->setFixedWidth(165);
  m_navigation->setSpacing(6);
  m_pages = new QStackedWidget;
  m_pages->addWidget(buildSearchPage());
  m_pages->addWidget(buildCatalogPage());
  m_pages->addWidget(buildSettingsPage());
  body->addWidget(m_navigation);
  body->addWidget(m_pages, 1);
  outer->addLayout(body, 1);
  setCentralWidget(central);
  connect(m_navigation, &QListWidget::currentRowChanged, m_pages, &QStackedWidget::setCurrentIndex);
  m_navigation->setCurrentRow(0);
  statusBar()->showMessage(QStringLiteral("配置目录：%1").arg(m_config.profilePath));
  fillSettings();
  m_pollTimer = new QTimer(this);
  m_pollTimer->setInterval(1200);
  connect(m_pollTimer, &QTimer::timeout, this, &MainWindow::pollJobs);
  m_pollTimer->start();
  if (!m_smokeTest) QTimer::singleShot(50, this, &MainWindow::checkService);
}

void MainWindow::reportStartupError(const QString &message)
{
  notice(QStringLiteral("配置读取失败：%1。请检查配置目录后重新保存。").arg(message), true);
}

QWidget *MainWindow::buildSearchPage()
{
  auto *page = new QWidget;
  auto *layout = new QVBoxLayout(page);
  layout->setContentsMargins(16, 0, 0, 0);
  layout->addWidget(heading(QStringLiteral("用自然语言查找工具")));
  m_query = new QPlainTextEdit;
  m_query->setObjectName(QStringLiteral("searchQuery"));
  m_query->setPlaceholderText(QStringLiteral("例如：将道路缓冲 500 米，再裁剪到行政区边界\n也可以输入英文需求或工具标识。"));
  m_query->setMaximumHeight(94);
  layout->addWidget(m_query);
  auto *options = new QHBoxLayout;
  options->addWidget(textLabel(QStringLiteral("候选数")));
  m_topN = new QSpinBox;
  m_topN->setObjectName(QStringLiteral("topN"));
  m_topN->setRange(1, 50);
  m_topN->setValue(8);
  options->addWidget(m_topN);
  options->addSpacing(14);
  options->addWidget(textLabel(QStringLiteral("目录来源")));
  m_sourceFilter = new QLineEdit;
  m_sourceFilter->setObjectName(QStringLiteral("sourceFilter"));
  m_sourceFilter->setPlaceholderText(QStringLiteral("留空搜索全部来源"));
  options->addWidget(m_sourceFilter, 1);
  m_searchButton = new QPushButton(QStringLiteral("检索工具"));
  m_searchButton->setObjectName(QStringLiteral("searchButton"));
  m_searchButton->setDefault(true);
  options->addWidget(m_searchButton);
  connect(m_searchButton, &QPushButton::clicked, this, &MainWindow::runSearch);
  layout->addLayout(options);
  m_searchProgress = new QProgressBar;
  m_searchProgress->setRange(0, 0);
  m_searchProgress->setMaximumHeight(5);
  m_searchProgress->hide();
  layout->addWidget(m_searchProgress);
  m_searchInfo = textLabel(QStringLiteral("检索得分用于候选排序，不代表执行成功概率。"));
  layout->addWidget(m_searchInfo);
  auto *splitter = new QSplitter(Qt::Vertical);
  m_candidates = new QTableWidget(0, 4);
  m_candidates->setObjectName(QStringLiteral("candidateTable"));
  m_candidates->setHorizontalHeaderLabels({QStringLiteral("工具 / 算法"), QStringLiteral("名称与说明"), QStringLiteral("融合得分"), QStringLiteral("语义相似度")});
  m_candidates->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
  m_candidates->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
  m_candidates->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
  m_candidates->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
  m_candidates->setSelectionBehavior(QAbstractItemView::SelectRows);
  m_candidates->setSelectionMode(QAbstractItemView::SingleSelection);
  m_candidates->setEditTriggers(QAbstractItemView::NoEditTriggers);
  m_candidates->setAlternatingRowColors(true);
  m_candidates->verticalHeader()->hide();
  splitter->addWidget(m_candidates);
  auto *details = new QWidget;
  auto *detailsLayout = new QVBoxLayout(details);
  detailsLayout->setContentsMargins(0, 5, 0, 0);
  auto *detailActions = new QHBoxLayout;
  detailActions->addWidget(textLabel(QStringLiteral("候选详情与原始 JSON")), 1);
  auto *fullRecord = new QPushButton(QStringLiteral("读取完整记录"));
  fullRecord->setObjectName(QStringLiteral("fullRecord"));
  auto *copy = new QPushButton(QStringLiteral("复制原始 JSON"));
  copy->setObjectName(QStringLiteral("copyOriginalJson"));
  detailActions->addWidget(fullRecord);
  detailActions->addWidget(copy);
  connect(fullRecord, &QPushButton::clicked, this, &MainWindow::fetchSelectedRecord);
  connect(copy, &QPushButton::clicked, this, [this] {
    const auto candidate = selectedCandidate();
    if (candidate.isEmpty()) return;
    const auto payload = candidate.value(QStringLiteral("payload"));
    const auto data = payload.isObject() ? jsonText(payload.toObject()) : jsonText(candidate);
    QApplication::clipboard()->setText(data);
    notice(QStringLiteral("已复制原始 JSON。检索不会执行其中的命令或确认标志。"));
  });
  detailsLayout->addLayout(detailActions);
  m_detail = new QPlainTextEdit;
  m_detail->setObjectName(QStringLiteral("candidateDetails"));
  m_detail->setReadOnly(true);
  m_detail->setPlaceholderText(QStringLiteral("选择候选，查看工具定义、调用示例及原始参数。"));
  detailsLayout->addWidget(m_detail);
  splitter->addWidget(details);
  splitter->setStretchFactor(0, 3);
  splitter->setStretchFactor(1, 2);
  layout->addWidget(splitter, 1);
  connect(m_candidates, &QTableWidget::itemSelectionChanged, this, &MainWindow::showSelectedCandidate);
  return page;
}

QWidget *MainWindow::buildCatalogPage()
{
  auto *page = new QWidget;
  auto *layout = new QVBoxLayout(page);
  layout->setContentsMargins(16, 0, 0, 0);
  layout->addWidget(heading(QStringLiteral("导入工具目录")));
  layout->addWidget(textLabel(QStringLiteral("支持版本化目录、MCP 工具列表、工具定义或调用示例 JSON。导入会替换所选来源的完整快照，其他来源保持不变。")));
  auto *form = new QFormLayout;
  m_sourceId = new QLineEdit(QStringLiteral("imported-tools"));
  m_sourceId->setObjectName(QStringLiteral("catalogSource"));
  form->addRow(QStringLiteral("来源标识"), m_sourceId);
  m_catalogFile = new QLineEdit;
  m_catalogFile->setObjectName(QStringLiteral("catalogFile"));
  form->addRow(QStringLiteral("JSON 文件"), fieldWithButton(m_catalogFile, QStringLiteral("选择文件…"), [this] {
    const auto file = QFileDialog::getOpenFileName(this, QStringLiteral("选择工具目录"), {}, QStringLiteral("JSON 文件 (*.json);;全部文件 (*)"));
    if (!file.isEmpty()) m_catalogFile->setText(file);
  }));
  layout->addLayout(form);
  auto *actions = new QHBoxLayout;
  auto *importButton = new QPushButton(QStringLiteral("导入并更新索引"));
  importButton->setObjectName(QStringLiteral("importCatalog"));
  auto *refresh = new QPushButton(QStringLiteral("刷新索引状态"));
  actions->addWidget(importButton);
  actions->addWidget(refresh);
  actions->addStretch();
  connect(importButton, &QPushButton::clicked, this, &MainWindow::importCatalog);
  connect(refresh, &QPushButton::clicked, this, &MainWindow::refreshIndex);
  layout->addLayout(actions);
  m_indexInfo = textLabel(QStringLiteral("数据库保存在本地。QGIS 和本程序可选择同一数据库文件。"));
  m_indexInfo->setObjectName(QStringLiteral("indexStatus"));
  layout->addWidget(m_indexInfo);
  layout->addSpacing(12);
  auto *jobActions = new QHBoxLayout;
  jobActions->addWidget(heading(QStringLiteral("本窗口提交的任务")), 1);
  auto *cancel = new QPushButton(QStringLiteral("取消所选任务"));
  cancel->setObjectName(QStringLiteral("cancelJob"));
  jobActions->addWidget(cancel);
  connect(cancel, &QPushButton::clicked, this, &MainWindow::cancelSelectedJob);
  layout->addLayout(jobActions);
  m_jobTable = new QTableWidget(0, 4);
  m_jobTable->setObjectName(QStringLiteral("jobTable"));
  m_jobTable->setHorizontalHeaderLabels({QStringLiteral("任务"), QStringLiteral("状态"), QStringLiteral("进度 / 说明"), QStringLiteral("任务 ID")});
  m_jobTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
  m_jobTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
  m_jobTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
  m_jobTable->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
  m_jobTable->setSelectionBehavior(QAbstractItemView::SelectRows);
  m_jobTable->setSelectionMode(QAbstractItemView::SingleSelection);
  m_jobTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
  m_jobTable->verticalHeader()->hide();
  m_jobTable->setAlternatingRowColors(true);
  layout->addWidget(m_jobTable, 1);
  return page;
}

QWidget *MainWindow::buildSettingsPage()
{
  auto *scroll = new QScrollArea;
  scroll->setWidgetResizable(true);
  scroll->setFrameShape(QFrame::NoFrame);
  auto *page = new QWidget;
  auto *layout = new QVBoxLayout(page);
  layout->setContentsMargins(16, 0, 10, 0);
  layout->addWidget(heading(QStringLiteral("模型与服务")));
  layout->addWidget(textLabel(QStringLiteral("本地推理与远端接口使用相同工具索引。服务地址不是向量身份，复用向量时模型版本也需要一致。")));
  auto *form = new QFormLayout;
  form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
  m_provider = new QComboBox;
  m_provider->setObjectName(QStringLiteral("embeddingProvider"));
  m_provider->addItem(QStringLiteral("内嵌 llama.cpp 本地推理"), QStringLiteral("embedded"));
  m_provider->addItem(QStringLiteral("HTTP 嵌入接口"), QStringLiteral("http"));
  form->addRow(QStringLiteral("嵌入来源"), m_provider);
  m_modelPath = new QLineEdit;
  m_modelPath->setObjectName(QStringLiteral("modelPath"));
  form->addRow(QStringLiteral("本地 GGUF"), fieldWithButton(m_modelPath, QStringLiteral("选择…"), [this] {
    const auto path = QFileDialog::getOpenFileName(this, QStringLiteral("选择已有 GGUF"), {}, QStringLiteral("GGUF 模型 (*.gguf)"));
    if (!path.isEmpty()) {
      if (QFileInfo(path).fileName() != QLatin1String("embeddinggemma-2-Q8_0.gguf")) {
        notice(QStringLiteral("这个文件名不是锁定模型的标准名称。请使用“导入并校验 GGUF”将文件校验并复制到模型目录。"), true);
        return;
      }
      m_modelPath->setText(path);
    }
  }));
  m_backend = new QComboBox;
  m_backend->addItem(QStringLiteral("CPU"), QStringLiteral("cpu"));
  m_backend->addItem(QStringLiteral("自动选择"), QStringLiteral("auto"));
  m_backend->addItem(QStringLiteral("指定 GPU"), QStringLiteral("gpu"));
  form->addRow(QStringLiteral("推理设备"), m_backend);
  m_deviceId = new QLineEdit;
  m_deviceId->setPlaceholderText(QStringLiteral("指定 GPU 时填写实际设备标识"));
  form->addRow(QStringLiteral("GPU 标识"), m_deviceId);
  m_threads = new QSpinBox;
  m_threads->setRange(1, 256);
  form->addRow(QStringLiteral("CPU 线程数"), m_threads);
  m_apiBase = new QLineEdit;
  m_apiBase->setObjectName(QStringLiteral("embeddingApiBase"));
  m_apiBase->setPlaceholderText(QStringLiteral("https://server.example/prefix/v1/"));
  form->addRow(QStringLiteral("嵌入 API 地址"), m_apiBase);
  m_authorization = new QLineEdit;
  m_authorization->setEchoMode(QLineEdit::Password);
  m_authorization->setPlaceholderText(QStringLiteral("可选，例如 Bearer …"));
  form->addRow(QStringLiteral("远端 Authorization"), m_authorization);
  m_modelId = new QLineEdit;
  form->addRow(QStringLiteral("模型 ID"), m_modelId);
  m_modelRevision = new QLineEdit;
  form->addRow(QStringLiteral("模型版本 / 指纹"), m_modelRevision);
  m_database = new QLineEdit;
  m_database->setObjectName(QStringLiteral("databasePath"));
  form->addRow(QStringLiteral("本地向量数据库"), fieldWithButton(m_database, QStringLiteral("选择…"), [this] {
    const auto path = QFileDialog::getSaveFileName(this, QStringLiteral("选择共享向量数据库"), m_database->text(), QStringLiteral("SQLite 数据库 (*.sqlite3 *.sqlite *.db);;全部文件 (*)"), nullptr, QFileDialog::DontConfirmOverwrite);
    if (!path.isEmpty()) m_database->setText(path);
  }));
  m_port = new QSpinBox;
  m_port->setObjectName(QStringLiteral("servicePort"));
  m_port->setRange(1, 65535);
  form->addRow(QStringLiteral("本机服务端口"), m_port);
  m_token = new QLineEdit;
  m_token->setObjectName(QStringLiteral("serviceToken"));
  m_token->setEchoMode(QLineEdit::Password);
  form->addRow(QStringLiteral("本机 API Token"), fieldWithButton(m_token, QStringLiteral("复制"), [this] {
    QApplication::clipboard()->setText(m_token->text());
    notice(QStringLiteral("已复制本机服务 Token，可用于 QGIS 的 HTTP 认证配置。"));
  }));
  m_timeout = new QSpinBox;
  m_timeout->setRange(1, 3600);
  m_timeout->setSuffix(QStringLiteral(" 秒"));
  form->addRow(QStringLiteral("请求超时"), m_timeout);
  layout->addLayout(form);
  auto *modelActions = new QHBoxLayout;
  m_downloadButton = new QPushButton(QStringLiteral("下载锁定模型"));
  m_importModelButton = new QPushButton(QStringLiteral("导入并校验 GGUF…"));
  auto *modelInfo = new QPushButton(QStringLiteral("检查模型"));
  modelActions->addWidget(m_downloadButton);
  modelActions->addWidget(m_importModelButton);
  modelActions->addWidget(modelInfo);
  modelActions->addStretch();
  layout->addLayout(modelActions);
  m_modelInfo = textLabel(QStringLiteral("模型文件不随安装包分发。本地模型缺失时仍可配置或使用远端接口。"));
  layout->addWidget(m_modelInfo);
  connect(m_downloadButton, &QPushButton::clicked, this, [this] {
    const auto active = m_serviceRunning ? m_connectedConfig : m_config;
    if (QFileInfo(active.modelPath).fileName() != QLatin1String("embeddinggemma-2-Q8_0.gguf")) {
      m_modelPath->setText(QDir(m_config.profilePath).filePath(QStringLiteral("models/embeddinggemma-2-Q8_0.gguf")));
      notice(QStringLiteral("已填写模型下载的标准路径。请保存设置并重新启动服务后再下载。"));
      return;
    }
    submitOperation(QStringLiteral("下载模型"), "POST", QStringLiteral("/v1/model/download"), {});
  });
  connect(m_importModelButton, &QPushButton::clicked, this, [this] {
    const auto active = m_serviceRunning ? m_connectedConfig : m_config;
    if (QFileInfo(active.modelPath).fileName() != QLatin1String("embeddinggemma-2-Q8_0.gguf")) {
      m_modelPath->setText(QDir(m_config.profilePath).filePath(QStringLiteral("models/embeddinggemma-2-Q8_0.gguf")));
      notice(QStringLiteral("已填写模型导入的标准路径。请保存设置并重新启动服务后再导入。"));
      return;
    }
    const auto path = QFileDialog::getOpenFileName(this, QStringLiteral("导入 GGUF 并校验完整性"), {}, QStringLiteral("GGUF 模型 (*.gguf)"));
    if (path.isEmpty()) return;
    submitOperation(QStringLiteral("导入模型"), "POST", QStringLiteral("/v1/model/import"), {{QStringLiteral("path"), path}});
  });
  connect(modelInfo, &QPushButton::clicked, this, &MainWindow::refreshModel);
  connect(m_provider, &QComboBox::currentIndexChanged, this, &MainWindow::updateProviderControls);
  connect(m_backend, &QComboBox::currentIndexChanged, this, &MainWindow::updateProviderControls);
  layout->addSpacing(10);
  auto *serviceActions = new QHBoxLayout;
  auto *save = new QPushButton(QStringLiteral("保存设置"));
  save->setObjectName(QStringLiteral("saveSettings"));
  auto *start = new QPushButton(QStringLiteral("启动服务"));
  start->setObjectName(QStringLiteral("startService"));
  auto *stop = new QPushButton(QStringLiteral("停止服务"));
  stop->setObjectName(QStringLiteral("stopService"));
  auto *profile = new QPushButton(QStringLiteral("打开配置目录"));
  serviceActions->addWidget(save);
  serviceActions->addWidget(start);
  serviceActions->addWidget(stop);
  serviceActions->addStretch();
  serviceActions->addWidget(profile);
  layout->addLayout(serviceActions);
  layout->addWidget(textLabel(QStringLiteral("服务端参数更改需重启。连接已有服务时，可保存正确的端口与 Token 后检查连接。关闭本窗口不会停止后台服务。")));
  layout->addStretch();
  connect(save, &QPushButton::clicked, this, [this] { saveSettings(); });
  connect(start, &QPushButton::clicked, this, &MainWindow::startService);
  connect(stop, &QPushButton::clicked, this, &MainWindow::stopService);
  connect(profile, &QPushButton::clicked, this, [this] {
    if (!QDir(m_config.profilePath).exists()) {
      notice(QStringLiteral("配置目录尚未创建，请先保存设置。"));
      return;
    }
    QDesktopServices::openUrl(QUrl::fromLocalFile(m_config.profilePath));
  });
  scroll->setWidget(page);
  return scroll;
}

void MainWindow::request(const QByteArray &method, const QString &path, const QJsonObject &body, ReplyHandler handler, int timeoutMs, const ServiceConfig *connectionOverride)
{
  const auto connection = connectionOverride ? *connectionOverride : m_serviceRunning ? m_connectedConfig : m_config;
  QUrl url;
  url.setScheme(QStringLiteral("http"));
  url.setHost(connection.host);
  url.setPort(connection.port);
  // Paths are already percent-encoded when they contain a source or job ID.
  url = QUrl::fromEncoded(url.toEncoded() + path.toUtf8());
  QNetworkRequest request(url);
  request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
  if (!connection.token.isEmpty()) request.setRawHeader("Authorization", QByteArray("Bearer ") + connection.token.toUtf8());
  request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
  const auto payload = QJsonDocument(body).toJson(QJsonDocument::Compact);
  QNetworkReply *reply = nullptr;
  if (method == "GET") reply = m_network->get(request);
  else if (method == "POST") reply = m_network->post(request, payload);
  else reply = m_network->sendCustomRequest(request, method, payload);
  auto *timer = new QTimer(reply);
  timer->setSingleShot(true);
  connect(timer, &QTimer::timeout, reply, [reply] { reply->setProperty("qctsmTimedOut", true); reply->abort(); });
  timer->start(timeoutMs > 0 ? timeoutMs : connection.timeoutMs);
  connect(reply, &QNetworkReply::finished, this, [reply, timer, handler = std::move(handler)] {
    timer->stop();
    const auto bytes = reply->readAll();
    QJsonParseError parse;
    const auto document = QJsonDocument::fromJson(bytes, &parse);
    QJsonObject result = document.isObject() ? document.object() : QJsonObject();
    QString error;
    const int httpCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (reply->property("qctsmTimedOut").toBool()) error = QStringLiteral("请求超时。后台任务未被自动取消。");
    else if (reply->error() != QNetworkReply::NoError || httpCode >= 400) {
      error = errorText(result);
      if (error.isEmpty()) error = reply->errorString();
      if (httpCode) error = QStringLiteral("HTTP %1：%2").arg(httpCode).arg(error);
    } else if (parse.error != QJsonParseError::NoError || !document.isObject()) error = QStringLiteral("服务未返回有效 JSON 对象。");
    else if (httpCode >= 300) error = QStringLiteral("服务返回重定向，未转发本机认证信息。");
    reply->deleteLater();
    handler(result, error);
  });
}

void MainWindow::notice(const QString &message, bool error)
{
  m_banner->setText(message);
  m_banner->setStyleSheet(error ? QStringLiteral("QLabel { color: #b3261e; }") : QString());
}

void MainWindow::checkService()
{
  m_connection->setText(QStringLiteral("正在检查服务…"));
  const auto connection = m_config;
  request("GET", QStringLiteral("/health"), {}, [this, connection](const QJsonObject &result, const QString &error) {
    const bool healthy = error.isEmpty() && result.value(QStringLiteral("status")).toString() == QLatin1String("ok")
      && result.value(QStringLiteral("service")).toString() == QLatin1String("qctsm");
    if (!healthy) {
      m_serviceRunning = false;
      m_connection->setText(QStringLiteral("服务未连接"));
      notice(QStringLiteral("无法连接本机服务。可在“模型与服务”中检查配置并启动。%1").arg(error), true);
      return;
    }
    // Health is intentionally public. Verify a protected endpoint before
    // pinning the connection, so a corrected token can be applied in-place.
    request("GET", QStringLiteral("/v1/models"), {}, [this, connection](const QJsonObject &models, const QString &authError) {
      m_serviceRunning = authError.isEmpty() && models.value(QStringLiteral("data")).isArray();
      if (!m_serviceRunning) {
        m_connection->setText(QStringLiteral("服务可达，认证未通过"));
        notice(QStringLiteral("请保存正确的 API Token，再点击“检查连接”。%1").arg(authError), true);
        return;
      }
      m_connectedConfig = connection;
      m_connection->setText(QStringLiteral("服务已连接 · %1:%2").arg(connection.host).arg(connection.port));
      refreshIndex();
      refreshModel();
    }, 5000, &connection);
  }, 5000, &connection);
}

void MainWindow::refreshIndex()
{
  request("GET", QStringLiteral("/v1/index/status"), {}, [this](const QJsonObject &result, const QString &error) {
    if (!error.isEmpty()) { m_indexInfo->setText(QStringLiteral("索引状态不可用：%1").arg(error)); return; }
    const auto object = result.value(QStringLiteral("result")).isObject() ? result.value(QStringLiteral("result")).toObject() : result;
    const auto count = object.value(QStringLiteral("record_count")).toInt(object.value(QStringLiteral("records")).toInt(-1));
    const auto generation = object.value(QStringLiteral("generation")).toVariant().toString();
    m_indexInfo->setText(QStringLiteral("数据库：%1\n记录数：%2    索引版本：%3").arg(m_connectedConfig.databasePath, count < 0 ? QStringLiteral("见服务详情") : QString::number(count), generation.isEmpty() ? QStringLiteral("未建立") : generation));
    m_indexInfo->setToolTip(jsonText(object));
  });
}

void MainWindow::refreshModel()
{
  request("GET", QStringLiteral("/v1/model/info"), {}, [this](const QJsonObject &result, const QString &error) {
    if (!error.isEmpty()) { m_modelInfo->setText(QStringLiteral("模型状态不可用：%1").arg(error)); return; }
    const auto provider = result.value(QStringLiteral("provider")).toString();
    const auto state = provider == QLatin1String("http") ? QStringLiteral("远端 HTTP 接口")
      : result.value(QStringLiteral("model_file_exists")).toBool() ? QStringLiteral("模型文件已存在，推理时验证完整性") : QStringLiteral("模型文件尚未下载或导入");
    m_modelInfo->setText(QStringLiteral("%1 · %2\n版本：%3").arg(result.value(QStringLiteral("model")).toString(), state, result.value(QStringLiteral("revision")).toString()));
    m_modelInfo->setToolTip(jsonText(result));
  });
}

void MainWindow::runSearch()
{
  if (m_searchPending) return;
  const auto query = m_query->toPlainText().trimmed();
  if (query.isEmpty()) { notice(QStringLiteral("请先输入要完成的任务或工具用途。"), true); m_query->setFocus(); return; }
  QJsonObject filters{{QStringLiteral("available"), true}};
  if (!m_sourceFilter->text().trimmed().isEmpty()) filters.insert(QStringLiteral("source_id"), m_sourceFilter->text().trimmed());
  m_searchPending = true;
  ++m_searchGeneration;
  m_searchButton->setEnabled(false);
  m_searchProgress->show();
  m_searchInfo->setText(QStringLiteral("正在匹配需求与工具目录…"));
  submitOperation(QStringLiteral("工具检索"), "POST", QStringLiteral("/v1/search"), {{QStringLiteral("query"), query}, {QStringLiteral("top_n"), m_topN->value()}, {QStringLiteral("filters"), filters}});
}

void MainWindow::importCatalog()
{
  QFile file(m_catalogFile->text().trimmed());
  if (!file.open(QIODevice::ReadOnly)) { notice(QStringLiteral("无法读取 JSON：%1").arg(file.errorString()), true); return; }
  if (file.size() > 16 * 1024 * 1024) { notice(QStringLiteral("单次 GUI 导入最大 16 MiB，请拆分为多个目录来源。"), true); return; }
  QJsonParseError parse;
  const auto document = QJsonDocument::fromJson(file.readAll(), &parse);
  if (parse.error != QJsonParseError::NoError || (!document.isObject() && !document.isArray())) { notice(QStringLiteral("JSON 无效：%1").arg(parse.errorString()), true); return; }
  const auto raw = document.isArray() ? QJsonValue(document.array())
    : document.object().value(QStringLiteral("tools")).isArray() ? document.object().value(QStringLiteral("tools")) : QJsonValue(document.object());
  const auto source = document.object().value(QStringLiteral("source_id")).toString(m_sourceId->text().trimmed());
  if (source.isEmpty()) { notice(QStringLiteral("请填写目录来源标识。"), true); return; }
  auto snapshot = normalizeCatalog(raw, source);
  if (snapshot.value(QStringLiteral("status")).toString() == QLatin1String("failed")) { notice(QStringLiteral("目录无效：") + errorText(snapshot), true); return; }
  m_sourceId->setText(source);
  snapshot.insert(QStringLiteral("source_id"), source);
  if (!snapshot.contains(QStringLiteral("revision"))) snapshot.insert(QStringLiteral("revision"), QUuid::createUuid().toString(QUuid::WithoutBraces));
  if (!snapshot.contains(QStringLiteral("schema_version"))) snapshot.insert(QStringLiteral("schema_version"), 1);
  submitOperation(QStringLiteral("更新工具目录"), "PUT", QStringLiteral("/v1/catalogs/") + QString::fromLatin1(QUrl::toPercentEncoding(source)), snapshot);
}

void MainWindow::submitOperation(const QString &purpose, const QByteArray &method, const QString &path, const QJsonObject &body)
{
  const auto connection = m_serviceRunning ? m_connectedConfig : m_config;
  const auto generation = purpose == QStringLiteral("工具检索") ? m_searchGeneration : 0;
  request(method, path, body, [this, purpose, connection, generation](const QJsonObject &result, const QString &error) {
    acceptOperation(purpose, result, error, connection, generation);
  }, 0, &connection);
}

void MainWindow::acceptOperation(const QString &purpose, const QJsonObject &result, const QString &error, const ServiceConfig &connection, quint64 searchGeneration)
{
  if (purpose == QStringLiteral("工具检索") && searchGeneration != m_searchGeneration) return;
  const auto failed = result.value(QStringLiteral("status")).toString() == QLatin1String("failed");
  if (!error.isEmpty() || failed) {
    notice(purpose + QStringLiteral("失败：") + (error.isEmpty() ? errorText(result) : error), true);
    if (purpose == QStringLiteral("工具检索")) { m_searchPending = false; m_searchButton->setEnabled(true); m_searchProgress->hide(); m_searchInfo->setText(QStringLiteral("检索失败，请检查服务与索引状态。")); }
    return;
  }
  QString id = result.value(QStringLiteral("job_id")).toString();
  if (id.isEmpty()) id = result.value(QStringLiteral("handle")).toObject().value(QStringLiteral("id")).toString();
  if (!id.isEmpty()) { trackJob(id, purpose, result, connection, searchGeneration); return; }
  if (purpose == QStringLiteral("工具检索")) { m_resultsConnection = connection; renderSearch(result.value(QStringLiteral("result")).isObject() ? result.value(QStringLiteral("result")).toObject() : result); }
  else { notice(purpose + QStringLiteral("已完成。")); refreshIndex(); refreshModel(); }
}

void MainWindow::trackJob(const QString &id, const QString &purpose, const QJsonObject &snapshot, const ServiceConfig &connection, quint64 searchGeneration)
{
  m_jobs.insert(id, {purpose, snapshot.value(QStringLiteral("status")).toString(QStringLiteral("queued")), snapshot, connection, searchGeneration});
  refreshJobTable();
  notice(QStringLiteral("%1已提交。任务 %2").arg(purpose, id));
  QTimer::singleShot(50, this, &MainWindow::pollJobs);
}

void MainWindow::pollJobs()
{
  if (m_smokeTest) return;
  const auto ids = m_jobs.keys();
  for (const auto &id : ids) {
    if (terminal(m_jobs.value(id).status) || m_polling.contains(id)) continue;
    m_polling.insert(id);
    request("GET", QStringLiteral("/v1/jobs/") + QString::fromLatin1(QUrl::toPercentEncoding(id)), {}, [this, id](const QJsonObject &result, const QString &error) {
      m_polling.remove(id);
      if (!m_jobs.contains(id)) return;
      auto &job = m_jobs[id];
      // A cancellation response may have completed this job while this poll
      // was in flight. Never replace that authoritative terminal state.
      if (terminal(job.status)) return;
      if (!error.isEmpty()) {
        job.snapshot.insert(QStringLiteral("message"), QStringLiteral("状态查询失败，任务状态未被判定为失败：") + error);
        refreshJobTable();
        return;
      }
      job.snapshot = result;
      job.status = result.value(QStringLiteral("status")).toString(result.value(QStringLiteral("state")).toString(QStringLiteral("running")));
      refreshJobTable();
      if (!terminal(job.status)) return;
      if (job.purpose == QStringLiteral("工具检索") && job.searchGeneration != m_searchGeneration) return;
      if (job.purpose == QStringLiteral("工具检索")) {
        m_searchPending = false;
        m_searchButton->setEnabled(true);
        m_searchProgress->hide();
      }
      if (job.status == QLatin1String("succeeded") || job.status == QLatin1String("completed")) {
        const auto content = result.value(QStringLiteral("result")).isObject() ? result.value(QStringLiteral("result")).toObject() : result;
        if (job.purpose == QStringLiteral("工具检索")) { m_resultsConnection = job.connection; renderSearch(content); }
        else { notice(job.purpose + QStringLiteral("已完成。")); refreshIndex(); refreshModel(); }
      } else notice(QStringLiteral("%1：%2。%3").arg(job.purpose, stateText(job.status), errorText(result)), job.status == QLatin1String("failed"));
    }, 10000, &m_jobs[id].connection);
  }
}

void MainWindow::refreshJobTable()
{
  const auto selected = selectedJobId();
  auto ids = m_jobs.keys();
  std::sort(ids.begin(), ids.end());
  m_jobTable->setRowCount(ids.size());
  for (int row = 0; row < ids.size(); ++row) {
    const auto &id = ids[row];
    const auto job = m_jobs.value(id);
    auto message = job.snapshot.value(QStringLiteral("message")).toString();
    if (message.isEmpty()) message = errorText(job.snapshot);
    const auto total = job.snapshot.value(QStringLiteral("total")).toInt();
    if (total > 0) message = QStringLiteral("%1 / %2").arg(job.snapshot.value(QStringLiteral("completed")).toInt()).arg(total);
    m_jobTable->setItem(row, 0, new QTableWidgetItem(job.purpose));
    m_jobTable->setItem(row, 1, new QTableWidgetItem(stateText(job.status)));
    m_jobTable->setItem(row, 2, new QTableWidgetItem(message));
    m_jobTable->setItem(row, 3, new QTableWidgetItem(id));
    if (id == selected) m_jobTable->selectRow(row);
  }
}

QString MainWindow::selectedJobId() const
{
  const auto row = m_jobTable->currentRow();
  return row < 0 || !m_jobTable->item(row, 3) ? QString() : m_jobTable->item(row, 3)->text();
}

void MainWindow::cancelSelectedJob()
{
  const auto id = selectedJobId();
  if (id.isEmpty()) { notice(QStringLiteral("请先选择一个正在执行的任务。")); return; }
  if (terminal(m_jobs.value(id).status)) { notice(QStringLiteral("所选任务已结束。")); return; }
  request("POST", QStringLiteral("/v1/jobs/") + QString::fromLatin1(QUrl::toPercentEncoding(id)) + QStringLiteral("/cancel"), {}, [this, id](const QJsonObject &result, const QString &error) {
    if (!error.isEmpty()) { notice(QStringLiteral("取消请求失败：") + error, true); return; }
    if (!m_jobs.contains(id) || terminal(m_jobs.value(id).status)) return;
    if (m_jobs.contains(id)) { m_jobs[id].status = result.value(QStringLiteral("status")).toString(QStringLiteral("cancelling")); m_jobs[id].snapshot = result; }
    if (m_jobs.value(id).purpose == QStringLiteral("工具检索") && m_jobs.value(id).searchGeneration == m_searchGeneration && terminal(m_jobs.value(id).status)) {
      m_searchPending = false;
      m_searchButton->setEnabled(true);
      m_searchProgress->hide();
      m_searchInfo->setText(QStringLiteral("检索任务已结束。"));
      if (m_jobs.value(id).status == QLatin1String("succeeded")) { m_resultsConnection = m_jobs.value(id).connection; renderSearch(result.value(QStringLiteral("result")).toObject()); }
    }
    refreshJobTable();
    notice(QStringLiteral("已请求取消，等待后台任务结束。"));
    pollJobs();
  }, 0, &m_jobs[id].connection);
}

void MainWindow::renderSearch(const QJsonObject &result)
{
  ++m_resultGeneration;
  m_searchPending = false;
  m_searchButton->setEnabled(true);
  m_searchProgress->hide();
  if (result.value(QStringLiteral("status")).toString() == QLatin1String("failed")) { notice(errorText(result), true); return; }
  const auto candidates = result.value(QStringLiteral("candidates")).toArray();
  m_candidates->setRowCount(candidates.size());
  for (int row = 0; row < candidates.size(); ++row) {
    const auto c = candidates[row].toObject();
    auto *id = new QTableWidgetItem(c.value(QStringLiteral("capability_id")).toString(c.value(QStringLiteral("record_id")).toString()));
    id->setData(Qt::UserRole, jsonText(c));
    m_candidates->setItem(row, 0, id);
    const auto name = c.value(QStringLiteral("name")).toString();
    const auto description = c.value(QStringLiteral("description")).toString();
    auto *summary = new QTableWidgetItem(name.isEmpty() ? description : name);
    summary->setToolTip(description);
    m_candidates->setItem(row, 1, summary);
    m_candidates->setItem(row, 2, new QTableWidgetItem(QString::number(c.value(QStringLiteral("score")).toDouble(), 'g', 5)));
    const auto semantic = c.value(QStringLiteral("semantic_score"));
    m_candidates->setItem(row, 3, new QTableWidgetItem(semantic.isDouble() ? QString::number(semantic.toDouble(), 'f', 4) : QStringLiteral("—")));
  }
  const auto mode = result.value(QStringLiteral("retrieval_mode")).toString();
  const auto modeText = mode == QLatin1String("lexical") ? QStringLiteral("关键词检索，未使用语义向量") : QStringLiteral("语义与关键词混合检索");
  m_searchInfo->setText(QStringLiteral("%1 个候选 · %2 · 索引版本 %3").arg(candidates.size()).arg(modeText, result.value(QStringLiteral("generation")).toVariant().toString()));
  if (!candidates.isEmpty()) m_candidates->selectRow(0);
  else m_detail->setPlainText(QStringLiteral("没有匹配候选。请检查目录来源与索引，或调整需求描述。"));
  const auto reason = result.value(QStringLiteral("degraded_reason")).toString(result.value(QStringLiteral("warning")).toString());
  notice(reason.isEmpty() ? QStringLiteral("检索完成。选择候选查看原始 JSON，匹配器不会执行工具。") : QStringLiteral("检索完成，当前为降级模式：") + reason);
}

QJsonObject MainWindow::selectedCandidate() const
{
  const int row = m_candidates->currentRow();
  if (row < 0 || !m_candidates->item(row, 0)) return {};
  return QJsonDocument::fromJson(m_candidates->item(row, 0)->data(Qt::UserRole).toString().toUtf8()).object();
}

void MainWindow::showSelectedCandidate()
{
  const auto record = selectedCandidate();
  if (!record.isEmpty()) m_detail->setPlainText(jsonText(record));
}

void MainWindow::fetchSelectedRecord()
{
  const auto candidate = selectedCandidate();
  const auto id = candidate.value(QStringLiteral("record_id")).toString();
  if (id.isEmpty()) { notice(QStringLiteral("请先选择一个候选。")); return; }
  const auto generation = m_resultGeneration;
  request("POST", QStringLiteral("/v1/records/get"), {{QStringLiteral("ids"), QJsonArray{id}}}, [this, id, generation](const QJsonObject &result, const QString &error) {
    if (generation != m_resultGeneration || selectedCandidate().value(QStringLiteral("record_id")).toString() != id) return;
    if (!error.isEmpty()) { notice(QStringLiteral("读取记录失败：") + error, true); return; }
    m_detail->setPlainText(jsonText(result));
  }, 0, &m_resultsConnection);
}

void MainWindow::fillSettings()
{
  m_provider->setCurrentIndex(std::max(0, m_provider->findData(m_config.provider)));
  m_modelPath->setText(m_config.modelPath);
  m_backend->setCurrentIndex(std::max(0, m_backend->findData(m_config.backend)));
  m_deviceId->setText(m_config.deviceId);
  m_threads->setValue(m_config.threads);
  m_apiBase->setText(m_config.apiBase);
  m_authorization->setText(m_config.authorization);
  m_modelId->setText(m_config.modelId);
  m_modelRevision->setText(m_config.modelRevision);
  m_database->setText(m_config.databasePath);
  m_port->setValue(m_config.port);
  m_token->setText(m_config.token);
  m_timeout->setValue(m_config.timeoutMs / 1000);
  updateProviderControls();
}

bool MainWindow::saveSettings()
{
  auto config = m_config;
  config.provider = m_provider->currentData().toString();
  config.modelPath = m_modelPath->text().trimmed();
  config.backend = m_backend->currentData().toString();
  config.deviceId = config.backend == QLatin1String("gpu") ? m_deviceId->text().trimmed() : QString();
  config.threads = m_threads->value();
  config.apiBase = m_apiBase->text().trimmed();
  config.authorization = m_authorization->text();
  config.modelId = m_modelId->text().trimmed();
  config.modelRevision = m_modelRevision->text().trimmed();
  config.databasePath = m_database->text().trimmed();
  config.port = m_port->value();
  config.token = m_token->text();
  config.timeoutMs = m_timeout->value() * 1000;
  if (config.databasePath.isEmpty() || config.modelId.isEmpty() || config.modelRevision.isEmpty()) { notice(QStringLiteral("请填写数据库路径、模型 ID 和模型版本。"), true); return false; }
  if (QDir::isRelativePath(config.databasePath)) config.databasePath = QDir(config.profilePath).absoluteFilePath(config.databasePath);
  if (config.provider == QLatin1String("http")) {
    QUrl url(config.apiBase);
    if (!url.isValid() || (url.scheme() != QLatin1String("http") && url.scheme() != QLatin1String("https")) || url.host().isEmpty() || !url.userInfo().isEmpty() || url.hasQuery() || url.hasFragment()) { notice(QStringLiteral("请填写有效 HTTP(S) API 地址，不含用户名、查询或片段。"), true); return false; }
    if (!url.path().endsWith(QLatin1Char('/'))) url.setPath(url.path() + QLatin1Char('/'));
    config.apiBase = url.toString();
  } else if (config.modelPath.isEmpty() || QFileInfo(config.modelPath).fileName() != QLatin1String("embeddinggemma-2-Q8_0.gguf")) {
    notice(QStringLiteral("本地模型路径须以 embeddinggemma-2-Q8_0.gguf 结尾。模型可在启动服务后下载或导入。"), true);
    return false;
  }
  QString error;
  if (!config.save(&error)) { notice(QStringLiteral("设置保存失败：") + error, true); return false; }
  // A separate connection snapshot keeps jobs and shutdown requests attached
  // to the running server while a different port/token is being configured.
  m_config = config;
  notice(m_serviceRunning ? QStringLiteral("设置已保存。正在运行的服务保持原配置，停止并重新启动后生效。") : QStringLiteral("设置已保存，可启动服务。"));
  return true;
}

void MainWindow::updateProviderControls()
{
  const bool embedded = m_provider->currentData().toString() == QLatin1String("embedded");
  m_modelPath->setEnabled(embedded);
  m_backend->setEnabled(embedded);
  m_deviceId->setEnabled(embedded && m_backend->currentData().toString() == QLatin1String("gpu"));
  m_threads->setEnabled(embedded);
  m_apiBase->setEnabled(!embedded);
  m_authorization->setEnabled(!embedded);
  m_downloadButton->setEnabled(embedded);
  m_importModelButton->setEnabled(embedded);
}

void MainWindow::startService()
{
  if (m_serviceRunning) { notice(QStringLiteral("当前服务已连接。若要应用新设置，请先停止服务。")); return; }
  if (!saveSettings()) return;
  QString error;
  m_config = ServiceConfig::load(m_config.profilePath, &error);
  if (!error.isEmpty()) { notice(error, true); return; }
  QString executable = QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("qctsm-service"));
#ifdef Q_OS_WIN
  executable += QStringLiteral(".exe");
#endif
  if (!QFileInfo::exists(executable)) { notice(QStringLiteral("找不到服务程序：%1").arg(executable), true); return; }
  qint64 pid = 0;
  QProcess process;
  process.setProgram(executable);
  process.setArguments({QStringLiteral("--profile"), m_config.profilePath});
  process.setWorkingDirectory(QCoreApplication::applicationDirPath());
#ifdef Q_OS_WIN
  process.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *arguments) {
    arguments->flags |= 0x08000000; // CREATE_NO_WINDOW: the native GUI owns the visible surface.
  });
#endif
  if (!process.startDetached(&pid)) { notice(QStringLiteral("服务启动失败，请检查安装目录和配置。"), true); return; }
  notice(QStringLiteral("已请求启动服务，进程 %1。启动日志位于配置目录中的 service.log。").arg(pid));
  QTimer::singleShot(1200, this, &MainWindow::checkService);
}

void MainWindow::stopService()
{
  request("POST", QStringLiteral("/v1/shutdown"), {}, [this](const QJsonObject &, const QString &error) {
    if (!error.isEmpty()) { notice(QStringLiteral("停止服务失败：") + error, true); return; }
    m_serviceRunning = false;
    m_connection->setText(QStringLiteral("服务正在停止"));
    notice(QStringLiteral("已请求停止服务。后台任务将按服务的取消与清理流程结束。"));
  }, 10000);
}
}
