// SPDX-License-Identifier: MIT
#pragma once
#include "service/service_config.h"
#include <QHash>
#include <QJsonObject>
#include <QMainWindow>
#include <QSet>
#include <functional>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QNetworkAccessManager;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class QSpinBox;
class QStackedWidget;
class QTableWidget;
class QTimer;

namespace qctsm {
class MainWindow final : public QMainWindow
{
  public:
    explicit MainWindow(const ServiceConfig &config, bool smokeTest = false, QWidget *parent = nullptr);
    void reportStartupError(const QString &message);

  private:
    using ReplyHandler = std::function<void(const QJsonObject &, const QString &)>;
    struct PendingJob { QString purpose; QString status; QJsonObject snapshot; ServiceConfig connection; quint64 searchGeneration = 0; };
    QWidget *buildSearchPage();
    QWidget *buildCatalogPage();
    QWidget *buildSettingsPage();
    void request(const QByteArray &method, const QString &path, const QJsonObject &body, ReplyHandler handler, int timeoutMs = 0, const ServiceConfig *connectionOverride = nullptr);
    void notice(const QString &message, bool error = false);
    void checkService();
    void refreshIndex();
    void refreshModel();
    void runSearch();
    void importCatalog();
    void submitOperation(const QString &purpose, const QByteArray &method, const QString &path, const QJsonObject &body);
    void acceptOperation(const QString &purpose, const QJsonObject &result, const QString &error, const ServiceConfig &connection, quint64 searchGeneration);
    void trackJob(const QString &id, const QString &purpose, const QJsonObject &snapshot, const ServiceConfig &connection, quint64 searchGeneration);
    void pollJobs();
    void refreshJobTable();
    void cancelSelectedJob();
    void renderSearch(const QJsonObject &result);
    void showSelectedCandidate();
    void fetchSelectedRecord();
    void fillSettings();
    bool saveSettings();
    void updateProviderControls();
    void startService();
    void stopService();
    QString selectedJobId() const;
    QJsonObject selectedCandidate() const;

    ServiceConfig m_config;
    ServiceConfig m_connectedConfig;
    ServiceConfig m_resultsConnection;
    QNetworkAccessManager *m_network = nullptr;
    QTimer *m_pollTimer = nullptr;
    QHash<QString, PendingJob> m_jobs;
    QSet<QString> m_polling;
    bool m_smokeTest = false;
    bool m_serviceRunning = false;
    bool m_searchPending = false;
    quint64 m_searchGeneration = 0;
    quint64 m_resultGeneration = 0;
    QListWidget *m_navigation = nullptr;
    QStackedWidget *m_pages = nullptr;
    QLabel *m_banner = nullptr;
    QLabel *m_connection = nullptr;
    QLabel *m_indexInfo = nullptr;
    QLabel *m_searchInfo = nullptr;
    QLabel *m_modelInfo = nullptr;
    QPlainTextEdit *m_query = nullptr;
    QSpinBox *m_topN = nullptr;
    QLineEdit *m_sourceFilter = nullptr;
    QComboBox *m_kindFilter = nullptr;
    QComboBox *m_availabilityFilter = nullptr;
    QPushButton *m_searchButton = nullptr;
    QProgressBar *m_searchProgress = nullptr;
    QTableWidget *m_candidates = nullptr;
    QPlainTextEdit *m_detail = nullptr;
    QLineEdit *m_catalogFile = nullptr;
    QLineEdit *m_sourceId = nullptr;
    QTableWidget *m_jobTable = nullptr;
    QComboBox *m_provider = nullptr;
    QLineEdit *m_modelPath = nullptr;
    QComboBox *m_backend = nullptr;
    QLineEdit *m_deviceId = nullptr;
    QSpinBox *m_threads = nullptr;
    QLineEdit *m_apiBase = nullptr;
    QLineEdit *m_authorization = nullptr;
    QLineEdit *m_modelId = nullptr;
    QLineEdit *m_modelRevision = nullptr;
    QLineEdit *m_database = nullptr;
    QSpinBox *m_port = nullptr;
    QLineEdit *m_token = nullptr;
    QSpinBox *m_timeout = nullptr;
    QPushButton *m_downloadButton = nullptr;
    QPushButton *m_importModelButton = nullptr;
};
}
