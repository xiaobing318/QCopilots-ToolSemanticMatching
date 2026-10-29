// SPDX-License-Identifier: MIT
#include "retrieval.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLockFile>
#include <QRegularExpression>
#include <QSet>
#include <QtEndian>
#include <sqlite3.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <numeric>
#include <stdexcept>

namespace qctsm
{
namespace
{
constexpr int SchemaVersion = 1;
constexpr int DocumentChunkChars = 480;
constexpr int EmbeddingBatchSize = 8;
constexpr int MaximumDocumentChars = 12000;

QString hash(const QByteArray &bytes)
{
  return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}
QByteArray json(const QJsonObject &value) { return QJsonDocument(value).toJson(QJsonDocument::Compact); }
QJsonObject failure(const QString &code, const QString &message)
{
  return {{"status", "failed"}, {"error_code", code}, {"message", message}};
}
bool cancelled(const std::atomic_bool *flag) { return flag && flag->load(std::memory_order_relaxed); }
QJsonObject cancellation() { return {{"status", "cancelled"}, {"error_code", "cancelled"}, {"message", "Operation cancelled."}}; }
class OperationCancelled : public std::exception {};
void checkCancellation(const std::atomic_bool *flag) { if (cancelled(flag)) throw OperationCancelled(); }
QString resolvedPath(const QString &path)
{
  const QFileInfo info(path);
  const QString canonical = info.canonicalFilePath();
  if (!canonical.isEmpty()) return canonical;
  const QString parent = QFileInfo(info.absolutePath()).canonicalFilePath();
  return QDir(parent.isEmpty() ? info.absolutePath() : parent).filePath(info.fileName());
}

class Database
{
  public:
    explicit Database(const QString &path)
    {
      const QFileInfo info(resolvedPath(path));
      if (path.isEmpty() || !QDir().mkpath(info.absolutePath()))
        throw std::runtime_error("Cannot create index directory.");
      if (sqlite3_open_v2(info.absoluteFilePath().toUtf8().constData(), &db,
                          SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nullptr) != SQLITE_OK)
      {
        const std::string error = db ? sqlite3_errmsg(db) : "Cannot open index database.";
        if (db) sqlite3_close(db);
        db = nullptr;
        throw std::runtime_error(error);
      }
      std::unique_ptr<sqlite3, decltype(&sqlite3_close)> constructionGuard(db, sqlite3_close);
      sqlite3_busy_timeout(db, 5000);
      exec("PRAGMA foreign_keys=ON");
      exec("PRAGMA synchronous=NORMAL");
      int foundVersion = scalar("PRAGMA user_version");
      if (foundVersion < 0 || foundVersion > SchemaVersion)
        throw std::runtime_error("Unsupported tool index database schema version.");
      if (foundVersion == 0)
      {
        QLockFile schemaLock(info.absoluteFilePath() + ".schema.lock");
        schemaLock.setStaleLockTime(0);
        if (!schemaLock.tryLock(5000)) throw std::runtime_error("Another process is initializing this index.");
        foundVersion = scalar("PRAGMA user_version");
        if (foundVersion == 0)
        {
          if (scalar("SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND name NOT LIKE 'sqlite_%'") != 0)
            throw std::runtime_error("The selected SQLite file is not a QCTSM index.");
          exec("PRAGMA journal_mode=WAL");
          exec("BEGIN IMMEDIATE");
          try
          {
            exec("CREATE TABLE qctsm_meta(key TEXT PRIMARY KEY,value INTEGER NOT NULL)");
            exec("INSERT INTO qctsm_meta VALUES('generation',0)");
            exec("CREATE TABLE qctsm_sources(source_id TEXT PRIMARY KEY,revision TEXT NOT NULL,snapshot_hash TEXT NOT NULL)");
            exec("CREATE TABLE qctsm_records("
           "source_id TEXT NOT NULL,record_id TEXT NOT NULL,capability_id TEXT NOT NULL,kind TEXT NOT NULL,"
           "record_json TEXT NOT NULL,document TEXT NOT NULL,document_hash TEXT NOT NULL,available INTEGER NOT NULL,"
           "PRIMARY KEY(source_id,record_id),FOREIGN KEY(source_id) REFERENCES qctsm_sources(source_id) ON DELETE CASCADE)");
            exec("CREATE TABLE qctsm_profiles(fingerprint TEXT PRIMARY KEY,profile_json TEXT NOT NULL)");
            exec("CREATE TABLE qctsm_vectors("
           "source_id TEXT NOT NULL,record_id TEXT NOT NULL,profile TEXT NOT NULL,document_hash TEXT NOT NULL,vector BLOB NOT NULL,"
           "PRIMARY KEY(source_id,record_id,profile),FOREIGN KEY(source_id,record_id) REFERENCES qctsm_records(source_id,record_id) ON DELETE CASCADE,"
           "FOREIGN KEY(profile) REFERENCES qctsm_profiles(fingerprint) ON DELETE CASCADE)");
            exec("CREATE INDEX qctsm_capabilities ON qctsm_records(capability_id)");
            exec("PRAGMA user_version=1");
            exec("COMMIT");
          }
          catch (...) { sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr); throw; }
        }
      }
      if (scalar("PRAGMA user_version") != SchemaVersion ||
          scalar("SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND name IN ('qctsm_meta','qctsm_sources','qctsm_records','qctsm_profiles','qctsm_vectors')") != 5)
        throw std::runtime_error("The selected SQLite file is not a supported QCTSM index.");
      if (scalarText("PRAGMA journal_mode") != "wal")
        throw std::runtime_error("This shared index requires WAL journal mode. Restore WAL in an exclusive maintenance session.");
      constructionGuard.release();
    }
    ~Database() { if (db) sqlite3_close(db); }
    Database(const Database &) = delete;
    void check(int code) const
    {
      if (code != SQLITE_OK && code != SQLITE_DONE && code != SQLITE_ROW)
        throw std::runtime_error(sqlite3_errmsg(db));
    }
    void exec(const char *sql) const { check(sqlite3_exec(db, sql, nullptr, nullptr, nullptr)); }
    int scalar(const char *sql) const
    {
      sqlite3_stmt *raw = nullptr;
      check(sqlite3_prepare_v2(db, sql, -1, &raw, nullptr));
      std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> statement(raw, sqlite3_finalize);
      const int code = sqlite3_step(raw);
      check(code);
      if (code != SQLITE_ROW) throw std::runtime_error("Index metadata is incomplete.");
      return sqlite3_column_int(raw, 0);
    }
    QString scalarText(const char *sql) const
    {
      sqlite3_stmt *raw = nullptr;
      check(sqlite3_prepare_v2(db, sql, -1, &raw, nullptr));
      std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> statement(raw, sqlite3_finalize);
      const int code = sqlite3_step(raw);
      check(code);
      if (code != SQLITE_ROW) throw std::runtime_error("Index metadata is incomplete.");
      return QString::fromUtf8(reinterpret_cast<const char *>(sqlite3_column_text(raw, 0)));
    }
    qint64 generation() const
    {
      sqlite3_stmt *s = nullptr;
      check(sqlite3_prepare_v2(db, "SELECT value FROM qctsm_meta WHERE key='generation'", -1, &s, nullptr));
      const int rc = sqlite3_step(s);
      const qint64 value = rc == SQLITE_ROW ? sqlite3_column_int64(s, 0) : 0;
      sqlite3_finalize(s);
      check(rc);
      return value;
    }
    void advance() const { exec("UPDATE qctsm_meta SET value=value+1 WHERE key='generation'"); }
    sqlite3 *db = nullptr;
};

class Statement
{
  public:
    Statement(Database &database, const char *sql): d(database) { d.check(sqlite3_prepare_v2(d.db, sql, -1, &s, nullptr)); }
    ~Statement() { sqlite3_finalize(s); }
    void bind(int index, const QString &value)
    {
      const QByteArray data = value.toUtf8();
      d.check(sqlite3_bind_text(s, index, data.constData(), data.size(), SQLITE_TRANSIENT));
    }
    void bind(int index, const QByteArray &value) { d.check(sqlite3_bind_blob(s, index, value.constData(), value.size(), SQLITE_TRANSIENT)); }
    void bind(int index, int value) { d.check(sqlite3_bind_int(s, index, value)); }
    bool next() { const int rc = sqlite3_step(s); d.check(rc); return rc == SQLITE_ROW; }
    QString text(int index) const
    {
      const auto *data = sqlite3_column_text(s, index);
      return data ? QString::fromUtf8(reinterpret_cast<const char *>(data), sqlite3_column_bytes(s, index)) : QString();
    }
    QByteArray blob(int index) const
    {
      return QByteArray(static_cast<const char *>(sqlite3_column_blob(s, index)), sqlite3_column_bytes(s, index));
    }
    int integer(int index) const { return sqlite3_column_int(s, index); }
    sqlite3_stmt *s = nullptr;
    Database &d;
};

class Transaction
{
  public:
    explicit Transaction(Database &database): d(database) { d.exec("BEGIN IMMEDIATE"); }
    ~Transaction() { if (!done) sqlite3_exec(d.db, "ROLLBACK", nullptr, nullptr, nullptr); }
    void commit() { d.exec("COMMIT"); done = true; }
  private:
    Database &d;
    bool done = false;
};

QString normalized(const QString &text) { return text.normalized(QString::NormalizationForm_KC).toCaseFolded(); }
bool cjk(QChar c)
{
  const ushort u = c.unicode();
  return (u >= 0x3400 && u <= 0x9fff) || (u >= 0xf900 && u <= 0xfaff);
}
QStringList tokenize(const QString &text)
{
  QStringList out;
  QString word, chinese;
  auto flushWord = [&]()
  {
    if (!word.isEmpty())
    {
      out.append(word);
      const auto parts = word.split(QRegularExpression("[_:./\\-]+"), Qt::SkipEmptyParts);
      if (parts.size() > 1) out.append(parts);
      word.clear();
    }
  };
  auto flushChinese = [&]()
  {
    if (chinese.size() == 1) out.append(chinese);
    for (int i = 0; i + 1 < chinese.size(); ++i) out.append(chinese.mid(i, 2));
    chinese.clear();
  };
  for (QChar c : normalized(text))
  {
    if (cjk(c)) { flushWord(); chinese.append(c); }
    else if (c.isLetterOrNumber() || QStringLiteral("_:.-/").contains(c)) { flushChinese(); word.append(c); }
    else { flushWord(); flushChinese(); }
  }
  flushWord(); flushChinese();
  return out;
}

QJsonValue semanticValue(const QJsonValue &value, int depth = 0)
{
  if (depth > 40) return QStringLiteral("[nested definition]");
  if (value.isObject())
  {
    QJsonObject out;
    const QJsonObject source = value.toObject();
    for (auto it = source.begin(); it != source.end(); ++it)
    {
      const QString key = it.key().toLower();
      if (key == "client_request_id" || key == "confirmed_risk" || key == "job_id" ||
          key.endsWith("confirmation_token") || key == "authorization" || key == "password" || key == "api_key") continue;
      out.insert(it.key(), semanticValue(it.value(), depth + 1));
    }
    return out;
  }
  if (value.isArray())
  {
    QJsonArray out;
    for (const auto &item : value.toArray()) out.append(semanticValue(item, depth + 1));
    return out;
  }
  if (value.isString())
  {
    const QString text = value.toString();
    static const QRegularExpression path(QStringLiteral("^(?:[A-Za-z]:[/\\\\]|/|\\\\\\\\)"));
    if (path.match(text).hasMatch())
    {
      const QString suffix = QFileInfo(QString(text).replace('\\', '/')).suffix().toLower();
      return suffix.isEmpty() ? QStringLiteral("<local-path>") : QStringLiteral("<local-path:.%1>").arg(suffix);
    }
  }
  return value;
}

QString semanticDocument(const QJsonObject &record, const QJsonObject &definition)
{
  QString text = record.value("name").toString() + '\n' + record.value("description").toString() + '\n';
  text += record.value("capability_id").toString() + '\n';
  for (const auto &tag : record.value("tags").toArray()) text += tag.toString() + ' ';
  text += '\n';
  if (!definition.isEmpty())
    text += definition.value("name").toString() + '\n' + definition.value("description").toString() + '\n';
  text += QString::fromUtf8(QJsonDocument(semanticValue(record.value("payload")).toObject()).toJson(QJsonDocument::Compact));
  // This is a retrieval projection. The complete schema and payload remain in record_json.
  return text.left(MaximumDocumentChars);
}

bool normalizeVector(QVector<float> &vector, int dimensions)
{
  if (vector.size() != dimensions || dimensions < 1) return false;
  double norm = 0;
  for (float value : vector) { if (!std::isfinite(value)) return false; norm += double(value) * value; }
  if (!std::isfinite(norm) || norm < 1e-20) return false;
  norm = std::sqrt(norm);
  for (float &value : vector) value = float(value / norm);
  return true;
}
QByteArray vectorBytes(const QVector<float> &vector)
{
  QByteArray bytes(vector.size() * int(sizeof(float)), Qt::Uninitialized);
  for (qsizetype i = 0; i < vector.size(); ++i)
  {
    quint32 bits;
    std::memcpy(&bits, &vector[i], sizeof(bits));
    qToLittleEndian(bits, bytes.data() + i * sizeof(bits));
  }
  return bytes;
}
QVector<float> readVector(const QByteArray &bytes, int dimensions)
{
  if (bytes.size() != dimensions * int(sizeof(float))) return {};
  QVector<float> out(dimensions);
  for (int i = 0; i < dimensions; ++i)
  {
    const quint32 bits = qFromLittleEndian<quint32>(bytes.constData() + i * sizeof(quint32));
    std::memcpy(&out[i], &bits, sizeof(bits));
  }
  if (!normalizeVector(out, dimensions)) return {};
  return out;
}
QString lockPath(const QString &path)
{
  return resolvedPath(path) + ".writer.lock";
}
}

QString EmbeddingProfile::fingerprint() const { return hash(json(toJson())); }
QJsonObject EmbeddingProfile::toJson() const
{
  return {{"model_id", modelId}, {"revision", revision}, {"dimensions", dimensions}, {"pooling", "mean"},
          {"normalization", "l2"}, {"prompt_version", "embeddinggemma-search-v1"}, {"document_version", "capability-json-v1"}};
}
EmbeddingProfile EmbeddingProfile::fromJson(const QJsonObject &object)
{
  EmbeddingProfile profile;
  profile.modelId = object.value("model_id").toString(profile.modelId);
  profile.revision = object.value("revision").toString(profile.revision);
  profile.dimensions = object.value("dimensions").toInt(profile.dimensions);
  return profile;
}

class RetrievalEngine::Impl
{
  public:
    QString path;
    std::shared_ptr<EmbeddingProvider> provider;
    EmbeddingProfile profile;
};
RetrievalEngine::RetrievalEngine(const QString &databasePath, std::shared_ptr<EmbeddingProvider> provider,
                               const EmbeddingProfile &profile): d(std::make_unique<Impl>())
{
  d->path = databasePath;
  d->provider = std::move(provider);
  d->profile = profile;
}
RetrievalEngine::~RetrievalEngine() = default;

QJsonObject RetrievalEngine::syncCatalog(const QJsonObject &snapshot, const std::atomic_bool *cancel,
                                       const std::function<void(qint64, qint64)> &progress)
{
  try
  {
    if (cancelled(cancel)) return cancellation();
    if (snapshot.value("schema_version").toInt() != 1 || snapshot.value("source_id").toString().trimmed().isEmpty() ||
        !snapshot.value("records").isArray() || !snapshot.value("revision").isString())
      return failure("invalid_catalog", "Catalog requires schema_version 1, source_id, revision and records.");
    if (d->profile.dimensions < 1 || d->profile.dimensions > 65536 || d->profile.modelId.isEmpty() || d->profile.revision.isEmpty())
      return failure("invalid_embedding_profile", "A model identity, revision and valid dimension are required.");
    if (snapshot.contains("expected_revision") && !snapshot.value("expected_revision").isNull() && !snapshot.value("expected_revision").isString())
      return failure("invalid_catalog", "expected_revision must be a string, null (source must be new), or omitted.");
    if (snapshot.contains("force_rebuild") && !snapshot.value("force_rebuild").isBool())
      return failure("invalid_catalog", "force_rebuild must be boolean.");
    const QString source = snapshot.value("source_id").toString();
    const QString revision = snapshot.value("revision").toString();
    const QJsonArray input = snapshot.value("records").toArray();
    if (input.size() > 50000) return failure("catalog_too_large", "Catalog has more than 50000 records. Split sources explicitly.");
    QList<QJsonObject> records;
    QSet<QString> seen;
    QHash<QString, QJsonObject> definitions;
    for (const auto &value : input)
    {
      if (!value.isObject()) return failure("invalid_record", "Every record must be a JSON object.");
      QJsonObject record = value.toObject();
      // A raw invocation object is a valid standalone example.
      if (!record.contains("payload"))
      {
        const bool isDefinition = record.contains("inputSchema") || record.contains("outputSchema") ||
                                  (record.contains("name") && record.contains("description") && !record.contains("arguments"));
        record = {{"kind", isDefinition ? "definition" : "example"}, {"payload", record}};
      }
      if (!record.value("payload").isObject()) return failure("invalid_record", "Record payload must be a JSON object.");
      const QString kind = record.value("kind").toString("definition");
      if (kind != "definition" && kind != "example") return failure("invalid_record_kind", "Record kind must be definition or example.");
      record.insert("kind", kind);
      if (json(record).size() > 1024 * 1024) return failure("record_too_large", "One JSON record exceeds 1 MiB.");
      QString id = record.value("record_id").toString();
      if (id.isEmpty()) id = hash(json(record));
      if (seen.contains(id)) return failure("duplicate_record_id", "A source snapshot contains duplicate record IDs.");
      seen.insert(id);
      record.insert("record_id", id);
      QString capability = record.value("capability_id").toString();
      const auto payload = record.value("payload").toObject();
      if (capability.isEmpty()) capability = payload.value("binary_id").toString();
      if (capability.isEmpty()) capability = payload.value("algorithm_id").toString();
      if (capability.isEmpty()) capability = payload.value("name").toString();
      if (capability.isEmpty()) capability = id;
      record.insert("capability_id", capability);
      if (!record.contains("name")) record.insert("name", payload.value("name").toString(capability));
      if (!record.contains("description")) record.insert("description", payload.value("description").toString());
      if (record.contains("available") && !record.value("available").isBool()) return failure("invalid_record", "available must be boolean.");
      if (!record.contains("available")) record.insert("available", true);
      records.append(record);
      if (kind == "definition") definitions.insert(capability, record);
    }
    for (auto &record : records)
    {
      if (record.value("kind") == "example")
      {
        const auto definition = definitions.value(record.value("capability_id").toString());
        if (!definition.isEmpty() && !definition.value("available").toBool(true)) record.insert("available", false);
      }
    }
    if (!QDir().mkpath(QFileInfo(d->path).absolutePath())) return failure("index_directory", "Cannot create index directory.");
    QLockFile writer(lockPath(d->path));
    writer.setStaleLockTime(0);
    if (!writer.tryLock(0)) return failure("index_busy", "Another process is updating this local index. Queries remain available.");
    Database db(d->path);
    const QString profile = d->profile.fingerprint();
    struct Pending { QString id, document, hash, title; };
    QList<Pending> pending;
    QJsonObject catalogContent = snapshot;
    catalogContent.remove("force_rebuild");
    catalogContent.remove("expected_revision");
    const QString snapshotHash = hash(json(catalogContent));
    const bool forceRebuild = snapshot.value("force_rebuild").toBool(false);
    {
      Transaction transaction(db);
      Statement existing(db, "SELECT revision,snapshot_hash FROM qctsm_sources WHERE source_id=?");
      existing.bind(1, source);
      const bool exists = existing.next();
      if (snapshot.contains("expected_revision"))
      {
        const QJsonValue expected = snapshot.value("expected_revision");
        if ((expected.isNull() && exists) || (expected.isString() && (!exists || existing.text(0) != expected.toString())))
          return failure("source_revision_conflict", "The current source revision differs from expected_revision. Reload before replacing this source.");
      }
      if (exists && existing.text(0) == revision && existing.text(1) != snapshotHash)
        return failure("source_revision_conflict", "This source revision was already used for different records. Increment revision.");
      Statement upsertSource(db, "INSERT INTO qctsm_sources VALUES(?,?,?) ON CONFLICT(source_id) DO UPDATE SET revision=excluded.revision,snapshot_hash=excluded.snapshot_hash");
      upsertSource.bind(1, source); upsertSource.bind(2, revision); upsertSource.bind(3, snapshotHash); upsertSource.next();
      Statement upsertProfile(db, "INSERT OR IGNORE INTO qctsm_profiles VALUES(?,?)");
      upsertProfile.bind(1, profile); upsertProfile.bind(2, QString::fromUtf8(json(d->profile.toJson()))); upsertProfile.next();
      QStringList obsolete;
      Statement all(db, "SELECT record_id FROM qctsm_records WHERE source_id=?"); all.bind(1, source);
      while (all.next()) if (!seen.contains(all.text(0))) obsolete.append(all.text(0));
      for (const QString &id : obsolete)
      {
        Statement remove(db, "DELETE FROM qctsm_records WHERE source_id=? AND record_id=?");
        remove.bind(1, source); remove.bind(2, id); remove.next();
      }
      for (const auto &record : records)
      {
        if (cancelled(cancel)) return cancellation();
        const QString id = record.value("record_id").toString();
        const QString document = semanticDocument(record, record.value("kind") == "example" ? definitions.value(record.value("capability_id").toString()) : QJsonObject());
        const QString documentHash = hash(document.toUtf8());
        Statement upsert(db, "INSERT INTO qctsm_records VALUES(?,?,?,?,?,?,?,?) ON CONFLICT(source_id,record_id) DO UPDATE SET capability_id=excluded.capability_id,kind=excluded.kind,record_json=excluded.record_json,document=excluded.document,document_hash=excluded.document_hash,available=excluded.available");
        upsert.bind(1, source); upsert.bind(2, id); upsert.bind(3, record.value("capability_id").toString());
        upsert.bind(4, record.value("kind").toString()); upsert.bind(5, QString::fromUtf8(json(record)));
        upsert.bind(6, document); upsert.bind(7, documentHash); upsert.bind(8, record.value("available").toBool() ? 1 : 0); upsert.next();
        Statement vector(db, "SELECT 1 FROM qctsm_vectors WHERE source_id=? AND record_id=? AND profile=? AND document_hash=?");
        vector.bind(1, source); vector.bind(2, id); vector.bind(3, profile); vector.bind(4, documentHash);
        const bool hasVector = vector.next();
        if ((forceRebuild || !hasVector) && record.value("available").toBool()) pending.append({id, document, documentHash, record.value("name").toString()});
      }
      db.advance();
      transaction.commit();
    }
    writer.unlock();
    // Metadata changes are visible before network work. Old vectors cannot match a new document hash.
    const qint64 total = std::count_if(records.cbegin(), records.cend(), [](const QJsonObject &record) { return record.value("available").toBool(); });
    const qint64 existing = total - pending.size();
    if (progress) progress(existing, total);
    int completed = 0;
    QString embeddingError;
    int recordIndex = 0;
    int documentOffset = 0;
    QHash<int, QVector<float>> sums;
    while (recordIndex < pending.size())
    {
      if (cancelled(cancel))
      {
        auto result = cancellation(); result.insert("metadata_applied", true); result.insert("embedded_records", completed); return result;
      }
      if (!d->provider) { embeddingError = "No embedding provider configured."; break; }
      QStringList texts;
      QList<int> owners;
      QList<int> completeRecords;
      while (texts.size() < EmbeddingBatchSize && recordIndex < pending.size())
      {
        const auto &record = pending[recordIndex];
        int length = std::min(DocumentChunkChars, int(record.document.size()) - documentOffset);
        if (length > 0 && documentOffset + length < record.document.size()
            && record.document.at(documentOffset + length - 1).isHighSurrogate()
            && record.document.at(documentOffset + length).isLowSurrogate()) --length;
        texts.append(QStringLiteral("title: %1 | text: %2").arg(record.title, record.document.mid(documentOffset, length)));
        owners.append(recordIndex);
        documentOffset += length;
        if (documentOffset >= record.document.size())
        {
          completeRecords.append(recordIndex);
          ++recordIndex;
          documentOffset = 0;
        }
      }
      QString error;
      auto vectors = d->provider->embed(texts, cancel, &error);
      if (cancelled(cancel))
      {
        auto result = cancellation(); result.insert("metadata_applied", true); result.insert("embedded_records", completed); return result;
      }
      // Validate the complete response before publishing any record from this batch.
      if (vectors.size() != texts.size()) embeddingError = error.isEmpty() ? "Embedding result has invalid count." : error;
      for (auto &vector : vectors)
        if (!normalizeVector(vector, d->profile.dimensions)) embeddingError = error.isEmpty() ? "Embedding result has invalid dimensions or values." : error;
      if (!embeddingError.isEmpty()) break;
      for (int fragment = 0; fragment < vectors.size(); ++fragment)
      {
        auto &sum = sums[owners[fragment]];
        if (sum.isEmpty()) sum.fill(0, d->profile.dimensions);
        for (int dimension = 0; dimension < sum.size(); ++dimension) sum[dimension] += vectors[fragment][dimension];
      }
      for (int owner : completeRecords)
        if (!normalizeVector(sums[owner], d->profile.dimensions)) embeddingError = "Document embedding is empty or zero.";
      if (!embeddingError.isEmpty()) break;
      if (completeRecords.isEmpty()) continue;
      if (!writer.tryLock(1000))
      {
        auto result = failure("index_busy", "Another process is publishing index changes. Retry this source snapshot.");
        result.insert("metadata_applied", true); result.insert("embedded_records", completed); return result;
      }
      {
        Transaction transaction(db);
        Statement currentSource(db, "SELECT revision,snapshot_hash FROM qctsm_sources WHERE source_id=?");
        currentSource.bind(1, source);
        if (!currentSource.next() || currentSource.text(0) != revision || currentSource.text(1) != snapshotHash)
        {
          auto result = failure("source_changed", "A newer source snapshot superseded these embeddings. They were not published.");
          result.insert("metadata_applied", true); result.insert("embedded_records", completed); return result;
        }
        for (int owner : completeRecords)
        {
          const auto &record = pending[owner];
          Statement insert(db, "INSERT INTO qctsm_vectors(source_id,record_id,profile,document_hash,vector) SELECT ?,?,?,?,? WHERE EXISTS(SELECT 1 FROM qctsm_records r JOIN qctsm_sources s ON s.source_id=r.source_id WHERE r.source_id=? AND r.record_id=? AND r.document_hash=? AND s.revision=? AND s.snapshot_hash=?) ON CONFLICT(source_id,record_id,profile) DO UPDATE SET document_hash=excluded.document_hash,vector=excluded.vector");
          insert.bind(1, source); insert.bind(2, record.id); insert.bind(3, profile); insert.bind(4, record.hash); insert.bind(5, vectorBytes(sums[owner]));
          insert.bind(6, source); insert.bind(7, record.id); insert.bind(8, record.hash); insert.bind(9, revision); insert.bind(10, snapshotHash); insert.next();
        }
        db.advance(); transaction.commit();
      }
      writer.unlock();
      completed += completeRecords.size();
      for (int owner : completeRecords) sums.remove(owner);
      if (progress) progress(existing + completed, total);
    }
    QJsonObject result{{"status", "succeeded"}, {"source_id", source}, {"revision", revision}, {"record_count", records.size()},
                       {"embedded_records", completed}, {"pending_records", pending.size() - completed}, {"generation", double(db.generation())},
                       {"embedding_profile", d->profile.toJson()}, {"metadata_applied", true}};
    if (!embeddingError.isEmpty())
    {
      result.insert("status", "failed"); result.insert("error_code", "embedding_failed"); result.insert("message", embeddingError);
      result.insert("retrieval_mode", "lexical");
    }
    return result;
  }
  catch (const std::exception &error) { return failure("index_error", QString::fromUtf8(error.what())); }
}

QJsonObject RetrievalEngine::search(const QJsonObject &request, const std::atomic_bool *cancel)
{
  try
  {
    if (cancelled(cancel)) return cancellation();
    const QString query = request.value("query").toString().trimmed();
    if (query.isEmpty() || query.size() > 8000) return failure("invalid_query", "Query must contain between 1 and 8000 characters.");
    const int limit = request.value("top_n").toInt(8);
    if (limit < 1 || limit > 100) return failure("invalid_top_n", "top_n must be between 1 and 100.");
    const auto filters = request.value("filters").toObject();
    if (filters.contains("available") && !filters.value("available").isBool()) return failure("invalid_filters", "available must be boolean.");
    const QString kind = filters.value("kind").toString();
    if (!kind.isEmpty() && kind != "definition" && kind != "example") return failure("invalid_filters", "kind must be definition or example.");
    Database db(d->path);
    // The rows and generation must come from one read snapshot even with another process indexing.
    qint64 generation = 0;
    struct Row { QString source, id, capability, document; QJsonObject record; QVector<float> vector; QHash<QString,int> terms; int length=0; double semantic=-2, lexical=0, fused=0; };
    QList<Row> rows;
    auto loadRows = [&]()
    {
      rows.clear();
      db.exec("BEGIN");
      generation = db.generation();
      Statement records(db, "SELECT r.source_id,r.record_id,r.capability_id,r.record_json,r.document,v.vector FROM qctsm_records r LEFT JOIN qctsm_vectors v ON r.source_id=v.source_id AND r.record_id=v.record_id AND r.document_hash=v.document_hash AND v.profile=? WHERE (?='' OR r.source_id=?) AND (?='' OR r.kind=?) AND r.available=? ORDER BY r.source_id,r.record_id");
      records.bind(1, d->profile.fingerprint()); const QString source = filters.value("source_id").toString();
      records.bind(2, source); records.bind(3, source); records.bind(4, kind); records.bind(5, kind);
      records.bind(6, filters.value("available").toBool(true) ? 1 : 0);
      while (records.next())
      {
        checkCancellation(cancel);
        Row row; row.source=records.text(0); row.id=records.text(1); row.capability=records.text(2);
        row.record=QJsonDocument::fromJson(records.text(3).toUtf8()).object(); row.document=records.text(4);
        row.vector=readVector(records.blob(5), d->profile.dimensions);
        for (const auto &term : tokenize(row.document)) { ++row.terms[term]; ++row.length; }
        rows.append(std::move(row));
      }
      db.exec("COMMIT");
    };
    loadRows();
    QString degradation;
    QVector<float> queryVector;
    const bool hasVector = std::any_of(rows.cbegin(), rows.cend(), [](const Row &r){return !r.vector.isEmpty();});
    if (hasVector && d->provider)
    {
      auto vectors=d->provider->embed({QStringLiteral("task: search result | query: ") + query}, cancel, &degradation);
      if (vectors.size()==1 && normalizeVector(vectors[0], d->profile.dimensions)) queryVector=vectors[0];
      else if (degradation.isEmpty()) degradation="Query embedding has invalid dimensions, count or values.";
    }
    else degradation = hasVector ? "No embedding provider configured." : "No compatible vectors are indexed for this embedding profile.";
    if (cancelled(cancel)) return cancellation();
    // Embedding HTTP can take seconds. Do not return capabilities deleted or disabled during that wait.
    if (db.generation() != generation) loadRows();
    const auto queryTerms = tokenize(query);
    QHash<QString,int> df;
    double averageLength=0;
    for (const auto &row : rows)
    {
      checkCancellation(cancel);
      averageLength += row.length;
      for (auto it=row.terms.cbegin();it!=row.terms.cend();++it) ++df[it.key()];
    }
    averageLength = rows.isEmpty() ? 1 : std::max(1.0, averageLength/rows.size());
    QSet<QString> uniqueTerms(queryTerms.cbegin(), queryTerms.cend());
    for (auto &row : rows)
    {
      checkCancellation(cancel);
      if (!queryVector.isEmpty() && !row.vector.isEmpty())
      {
        row.semantic=0;
        for (int i=0;i<queryVector.size();++i) row.semantic+=double(queryVector[i])*row.vector[i];
      }
      for (const QString &term : uniqueTerms)
      {
        const int tf=row.terms.value(term);
        if (!tf) continue;
        const double idf=std::log(1+(rows.size()-df.value(term)+0.5)/(df.value(term)+0.5));
        row.lexical+=idf*(tf*2.2)/(tf+1.2*(0.25+0.75*row.length/averageLength));
      }
      if (normalized(query)==normalized(row.capability) || normalized(query)==normalized(row.record.value("name").toString())) row.lexical+=100;
    }
    QList<int> semanticOrder, lexicalOrder;
    for (int i=0;i<rows.size();++i) { if (rows[i].semantic>-2) semanticOrder.append(i); if (rows[i].lexical>0) lexicalOrder.append(i); }
    auto rank=[&](QList<int> &order, bool semantic)
    {
      std::stable_sort(order.begin(),order.end(),[&](int a,int b){return semantic ? rows[a].semantic>rows[b].semantic : rows[a].lexical>rows[b].lexical;});
      for (int i=0;i<order.size();++i) rows[order[i]].fused+=1.0/(61+i);
    };
    rank(semanticOrder,true); rank(lexicalOrder,false);
    QList<int> order;
    for(int i=0;i<rows.size();++i) if(rows[i].fused>0) order.append(i);
    std::stable_sort(order.begin(),order.end(),[&](int a,int b){return rows[a].fused>rows[b].fused;});
    QJsonArray candidates;
    QSet<QString> groups;
    for(int i : order)
    {
      checkCancellation(cancel);
      const auto &best=rows[i];
      const QString key=best.source+'\n'+best.capability;
      if(groups.contains(key)) continue;
      groups.insert(key);
      const Row *display=&best;
      for(const auto &row:rows)
        if(row.source==best.source && row.capability==best.capability && row.record.value("kind")=="definition") { display=&row;break; }
      QJsonArray matched;
      for(int j:order)
      {
        const auto &row=rows[j];
        if(row.source==best.source && row.capability==best.capability && matched.size()<8)
          matched.append(QJsonObject{{"record_id",row.id},{"kind",row.record.value("kind")},{"score",row.fused},{"payload",row.record.value("payload")}});
      }
      QJsonObject candidate{{"source_id",best.source},{"capability_id",best.capability},{"record_id",display->id},
                            {"name",display->record.value("name")},{"description",display->record.value("description")},
                            {"score",best.fused},{"lexical_score",best.lexical},{"payload",display->record.value("payload")},
                            {"matched_records",matched},{"metadata",display->record.value("metadata")}};
      candidate.insert("semantic_score",best.semantic>-2 ? QJsonValue(best.semantic) : QJsonValue(QJsonValue::Null));
      candidates.append(candidate);
      if(candidates.size()>=limit) break;
    }
    QJsonObject result{{"status","succeeded"},{"generation",double(generation)},{"embedding_profile",d->profile.toJson()},
                       {"retrieval_mode",queryVector.isEmpty()?"lexical":"hybrid"},{"candidates",candidates},
                       {"record_count",rows.size()},{"score_kind","rrf_not_probability"}};
    if(!degradation.isEmpty()) result.insert("degradation_reason",degradation);
    return result;
  }
  catch(const OperationCancelled &){return cancellation();}
  catch(const std::exception &error){return failure("search_error",QString::fromUtf8(error.what()));}
}

QJsonObject RetrievalEngine::getRecords(const QStringList &ids)
{
  try
  {
    if(ids.isEmpty() || ids.size()>100) return failure("invalid_record_ids","Provide between 1 and 100 record or capability IDs.");
    Database db(d->path);
    db.exec("BEGIN");
    const qint64 generation = db.generation();
    QJsonArray records;
    QSet<QString> returned;
    for(const auto &id:ids)
    {
      Statement select(db,"SELECT source_id,record_id,record_json FROM qctsm_records WHERE record_id=? OR capability_id=? ORDER BY source_id,record_id");
      select.bind(1,id);select.bind(2,id);
      while(select.next())
      {
        const QString key=select.text(0)+'\n'+select.text(1);
        if(returned.contains(key)) continue;
        returned.insert(key);
        QJsonObject record=QJsonDocument::fromJson(select.text(2).toUtf8()).object();
        record.insert("source_id",select.text(0));records.append(record);
      }
    }
    db.exec("COMMIT");
    return {{"status","succeeded"},{"generation",double(generation)},{"records",records}};
  }
  catch(const std::exception &error){return failure("index_error",QString::fromUtf8(error.what()));}
}

QJsonObject RetrievalEngine::status()
{
  try
  {
    Database db(d->path);
    db.exec("BEGIN");
    const qint64 generation = db.generation();
    Statement counts(db,"SELECT COUNT(*),SUM(available) FROM qctsm_records");counts.next();
    const int count=counts.integer(0), available=counts.integer(1);
    Statement vectors(db,"SELECT COUNT(*) FROM qctsm_vectors v JOIN qctsm_records r ON r.source_id=v.source_id AND r.record_id=v.record_id AND r.document_hash=v.document_hash WHERE v.profile=? AND r.available=1");
    vectors.bind(1,d->profile.fingerprint());vectors.next();
    QJsonArray sources;
    Statement source(db,"SELECT source_id,revision FROM qctsm_sources ORDER BY source_id");
    while(source.next()) sources.append(QJsonObject{{"source_id",source.text(0)},{"revision",source.text(1)}});
    db.exec("COMMIT");
    return {{"status","succeeded"},{"schema_version",SchemaVersion},{"generation",double(generation)},
            {"record_count",count},{"available_records",available},{"embedded_records",vectors.integer(0)},
            {"embedding_profile",d->profile.toJson()},{"sources",sources},{"database_path",QFileInfo(d->path).absoluteFilePath()}};
  }
  catch(const std::exception &error){return failure("index_error",QString::fromUtf8(error.what()));}
}
}
