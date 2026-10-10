// ============================================================================
//  HistoryService.cpp — SQLite 历史记录实现
//
//  需求依据：SRS V1.1 4.10、7.2、7.3、9.2
// ============================================================================
#include "services/HistoryService.h"

#include "core/BuildConfig.h"
#include "core/ErrorCode.h"
#include "core/Logger.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSqlError>
#include <QSqlQuery>
#include <QSqlRecord>
#include <QUuid>
#include <QVariant>

namespace dc {

namespace {

constexpr int kSchemaVersion = 1;

// ISO 时间字符串 -> QDateTime
QDateTime parseTime(const QVariant &value)
{
    if (value.isNull())
        return QDateTime();

    if (value.typeId() == QMetaType::QDateTime)
        return value.toDateTime();

    return QDateTime::fromString(value.toString(), Qt::ISODate);
}

// 构造 SQL 的 IN 占位符
QString placeholders(int count)
{
    QStringList out;
    for (int i = 0; i < count; ++i)
        out.append(QStringLiteral("?"));
    return out.join(QLatin1Char(','));
}

} // namespace

// ===========================================================================
//  构造 / 析构
// ===========================================================================
HistoryService::HistoryService(QObject *parent)
    : QObject(parent)
{
    // 每个实例使用独立的连接名，避免多实例互相干扰
    m_connectionName = QStringLiteral("dc_history_")
                       + QUuid::createUuid().toString(QUuid::WithoutBraces);
}

HistoryService::~HistoryService()
{
    close();
}

// ===========================================================================
//  初始化
// ===========================================================================
bool HistoryService::initialize(const QString &databasePath)
{
    if (m_available)
        close();

    m_databasePath = databasePath;
    m_lastError.clear();

    if (m_databasePath.isEmpty()) {
        m_lastError = QStringLiteral("未指定数据库路径。");
        DC_ERROR(LogCat::History, m_lastError);
        return false;
    }

    // 确保目录存在
    const QFileInfo info(m_databasePath);
    QDir dir = info.absoluteDir();
    if (!dir.exists() && !dir.mkpath(QStringLiteral("."))) {
        m_lastError = QStringLiteral("无法创建历史数据目录：%1")
                          .arg(dir.absolutePath());
        DC_ERROR(LogCat::History, m_lastError);
        return false;
    }

    // ---- 打开数据库 ----
    // 若打开失败（常见原因：文件损坏、被其他进程独占），
    // 尝试备份并重建，保证程序能继续运行（SRS 7.2）
    auto openDatabase = [this]() -> bool {
        if (QSqlDatabase::contains(m_connectionName))
            QSqlDatabase::removeDatabase(m_connectionName);

        m_db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), m_connectionName);
        m_db.setDatabaseName(m_databasePath);

        if (!m_db.open()) {
            m_lastError = QStringLiteral("无法打开历史数据库：%1")
                              .arg(m_db.lastError().text());
            return false;
        }
        return true;
    };

    if (!openDatabase()) {
        // 尝试备份损坏文件后重建
        if (QFile::exists(m_databasePath)) {
            const QString backup = m_databasePath + QStringLiteral(".corrupted.")
                + QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss"));

            if (QFile::copy(m_databasePath, backup)) {
                DC_WARN(LogCat::History,
                        QStringLiteral("历史数据库无法打开，已备份至 %1 并尝试重建。")
                            .arg(backup));
                QFile::remove(m_databasePath);
            }
        }

        if (!openDatabase()) {
            m_available = false;
            DC_ERROR(LogCat::History,
                     QStringLiteral("历史数据库不可用，历史功能已禁用：%1").arg(m_lastError));
            return false;
        }
    }

    // ---- PRAGMA 设置 ----
    //  WAL 模式：读写并发更好，异常退出时数据更安全
    //  foreign_keys：本服务未使用外键，但打开它以保持规范
    //  synchronous=NORMAL：在 WAL 下兼顾安全与性能
    execSimple(QStringLiteral("PRAGMA journal_mode = WAL"));
    execSimple(QStringLiteral("PRAGMA synchronous = NORMAL"));
    execSimple(QStringLiteral("PRAGMA foreign_keys = ON"));
    // 忙等超时：避免多进程访问时立即报错
    m_db.setConnectOptions(QStringLiteral("QSQLITE_BUSY_TIMEOUT=3000"));

    // ---- 建表 ----
    if (!createSchema()) {
        m_available = false;
        DC_ERROR(LogCat::History,
                 QStringLiteral("历史数据库建表失败：%1").arg(m_lastError));
        return false;
    }

    // ---- 完整性自检 ----
    QString integrityDetail;
    if (!verifyIntegrity(&integrityDetail)) {
        DC_WARN(LogCat::History,
                QStringLiteral("历史数据库自检发现问题并已处理：%1").arg(integrityDetail));
    }

    m_available = true;
    DC_INFO(LogCat::History,
            QStringLiteral("历史数据库已就绪：%1").arg(databasePath));

    return true;
}

void HistoryService::close()
{
    if (m_db.isOpen())
        m_db.close();

    m_db = QSqlDatabase();

    if (QSqlDatabase::contains(m_connectionName))
        QSqlDatabase::removeDatabase(m_connectionName);

    m_available = false;
}

bool HistoryService::isAvailable() const
{
    return m_available;
}

QString HistoryService::lastError() const
{
    return m_lastError;
}

void HistoryService::setPrivacySettings(const PrivacySettings &settings)
{
    m_privacy = settings;
}

PrivacySettings HistoryService::privacySettings() const
{
    return m_privacy;
}

void HistoryService::setRecordingsDirectory(const QString &dir)
{
    m_recordingsDir = dir;
}

QString HistoryService::recordingsDirectory() const
{
    return m_recordingsDir;
}

// ===========================================================================
//  建表
// ===========================================================================
bool HistoryService::createSchema()
{
    // 读取当前的 schema 版本
    const int existingVersion = currentSchemaVersion();

    // 表定义与 SRS 4.10 的表格逐列对齐
    const QStringList ddl{
        // ---- conversation ----
        QStringLiteral(R"SQL(
            CREATE TABLE IF NOT EXISTS conversation (
                id          INTEGER PRIMARY KEY AUTOINCREMENT,
                created_at  TEXT    NOT NULL,
                role        TEXT    NOT NULL,
                content     TEXT    NOT NULL,
                language    TEXT,
                session_id  TEXT
            )
        )SQL"),

        // ---- task_history ----
        QStringLiteral(R"SQL(
            CREATE TABLE IF NOT EXISTS task_history (
                id              INTEGER PRIMARY KEY AUTOINCREMENT,
                created_at      TEXT    NOT NULL,
                command_id      TEXT,
                parameters      TEXT,
                status          TEXT,
                result_summary  TEXT
            )
        )SQL"),

        // ---- translation_history ----
        QStringLiteral(R"SQL(
            CREATE TABLE IF NOT EXISTS translation_history (
                id               INTEGER PRIMARY KEY AUTOINCREMENT,
                created_at       TEXT    NOT NULL,
                source_lang      TEXT,
                target_lang      TEXT,
                source_text      TEXT,
                translated_text  TEXT,
                provider_id      TEXT
            )
        )SQL"),

        // ---- search_history ----
        QStringLiteral(R"SQL(
            CREATE TABLE IF NOT EXISTS search_history (
                id           INTEGER PRIMARY KEY AUTOINCREMENT,
                created_at   TEXT    NOT NULL,
                query        TEXT    NOT NULL,
                provider_id  TEXT
            )
        )SQL"),

        // ---- recording_metadata ----
        QStringLiteral(R"SQL(
            CREATE TABLE IF NOT EXISTS recording_metadata (
                id          INTEGER PRIMARY KEY AUTOINCREMENT,
                created_at  TEXT    NOT NULL,
                file_path   TEXT,
                duration    INTEGER DEFAULT 0,
                format      TEXT,
                deleted     INTEGER DEFAULT 0
            )
        )SQL"),

        // ---- 元数据表（记录 schema 版本，供迁移用）----
        QStringLiteral(R"SQL(
            CREATE TABLE IF NOT EXISTS dc_meta (
                key    TEXT PRIMARY KEY,
                value  TEXT
            )
        )SQL"),

        // ---- 索引（提升检索性能，SRS 4.10 检索场景）----
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_conversation_created "
                       "ON conversation(created_at DESC)"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_conversation_session "
                       "ON conversation(session_id)"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_task_created "
                       "ON task_history(created_at DESC)"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_task_command "
                       "ON task_history(command_id)"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_translation_created "
                       "ON translation_history(created_at DESC)"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_search_created "
                       "ON search_history(created_at DESC)"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_recording_created "
                       "ON recording_metadata(created_at DESC)"),
    };

    for (const QString &sql : ddl) {
        if (!execSimple(sql)) {
            return false;
        }
    }

    // 写入 / 更新 schema 版本
    if (existingVersion == 0) {
        QSqlQuery query(m_db);
        query.prepare(QStringLiteral(
            "INSERT OR REPLACE INTO dc_meta (key, value) VALUES (?, ?)"));
        query.addBindValue(QString::fromLatin1(kSchemaVersionKey));
        query.addBindValue(QString::number(kSchemaVersion));
        if (!query.exec()) {
            m_lastError = query.lastError().text();
            return false;
        }
        DC_INFO(LogCat::History,
                QStringLiteral("历史数据库已创建（schema v%1）").arg(kSchemaVersion));
    } else if (existingVersion < kSchemaVersion) {
        if (!applyMigrations(existingVersion))
            return false;
    } else if (existingVersion > kSchemaVersion) {
        // 数据库比程序新：不降级、不破坏，仅提示
        DC_WARN(LogCat::History,
                QStringLiteral("历史数据库版本（v%1）高于程序支持的版本（v%2），"
                               "将以兼容方式使用，不会修改表结构。")
                    .arg(existingVersion).arg(kSchemaVersion));
    }

    return true;
}

int HistoryService::currentSchemaVersion() const
{
    if (!m_db.isOpen())
        return 0;

    QSqlQuery query(m_db);
    if (!query.exec(QStringLiteral("SELECT value FROM dc_meta WHERE key = 'schema_version'")))
        return 0;

    if (!query.next())
        return 0;

    return query.value(0).toInt();
}

bool HistoryService::applyMigrations(int fromVersion)
{
    DC_INFO(LogCat::History,
            QStringLiteral("开始迁移历史数据库：v%1 -> v%2")
                .arg(fromVersion).arg(kSchemaVersion));

    // 本版本尚无历史迁移；未来在此处按版本递增执行 ALTER TABLE。
    // 结构示例：
    //   if (fromVersion < 2) { execSimple("ALTER TABLE ... ADD COLUMN ..."); }

    QSqlQuery query(m_db);
    query.prepare(QStringLiteral(
        "INSERT OR REPLACE INTO dc_meta (key, value) VALUES (?, ?)"));
    query.addBindValue(QString::fromLatin1(kSchemaVersionKey));
    query.addBindValue(QString::number(kSchemaVersion));

    if (!query.exec()) {
        m_lastError = query.lastError().text();
        DC_ERROR(LogCat::History,
                 QStringLiteral("历史数据库迁移失败：%1").arg(m_lastError));
        return false;
    }

    return true;
}

bool HistoryService::tableExists(const QString &tableName) const
{
    if (!m_db.isOpen())
        return false;

    QSqlQuery query(m_db);
    query.prepare(QStringLiteral(
        "SELECT name FROM sqlite_master WHERE type='table' AND name=?"));
    query.addBindValue(tableName);

    if (!query.exec())
        return false;

    return query.next();
}

bool HistoryService::execSimple(const QString &sql, QString *error) const
{
    if (!m_db.isOpen()) {
        if (error)
            *error = QStringLiteral("数据库未打开。");
        return false;
    }

    QSqlQuery query(m_db);
    if (!query.exec(sql)) {
        const QString err = query.lastError().text();
        if (error)
            *error = err;
        const_cast<HistoryService *>(this)->m_lastError = err;

        DC_WARN(LogCat::History,
                QStringLiteral("SQL 执行失败：%1 | %2").arg(err, sql.left(80)));
        return false;
    }

    return true;
}

// ===========================================================================
//  隐私开关
// ===========================================================================
bool HistoryService::isCategoryEnabled(HistoryCategory category) const
{
    return m_privacy.isCategoryEnabled(category);
}

Result HistoryService::privacyDisabledResult(HistoryCategory category) const
{
    return Result::failure(
        erc::HistoryDisabled,
        QStringLiteral("「%1」历史记录已关闭，本次未保存。可在设置中重新开启。")
            .arg(historyCategoryToString(category)));
}

// ===========================================================================
//  写入
// ===========================================================================
namespace {

// 统一的写入前置检查
bool writable(const HistoryService *self, HistoryCategory category, Result *out)
{
    if (!self->isAvailable()) {
        *out = Result::failure(erc::HistoryDbOpenFailed,
                               QStringLiteral("历史数据库不可用，本次未保存。"));
        return false;
    }
    if (!self->privacySettings().isCategoryEnabled(category)) {
        *out = Result::failure(
            erc::HistoryDisabled,
            QStringLiteral("「%1」历史记录已关闭，本次未保存。")
                .arg(historyCategoryToString(category)));
        return false;
    }
    return true;
}

} // namespace

Result HistoryService::addConversation(const ConversationRecord &record)
{
    Result guard;
    if (!writable(this, HistoryCategory::Conversation, &guard))
        return guard;

    QSqlQuery query(m_db);
    query.prepare(QStringLiteral(
        "INSERT INTO conversation (created_at, role, content, language, session_id) "
        "VALUES (?, ?, ?, ?, ?)"));

    const QDateTime created = record.createdAt.isValid()
        ? record.createdAt : QDateTime::currentDateTime();

    query.addBindValue(created.toString(Qt::ISODate));
    query.addBindValue(record.role);
    query.addBindValue(record.content);
    query.addBindValue(record.language);
    query.addBindValue(record.sessionId);

    if (!query.exec()) {
        m_lastError = query.lastError().text();
        return Result::failure(erc::HistoryQueryFailed,
                               QStringLiteral("保存对话记录失败。"), m_lastError);
    }

    emit historyChanged(HistoryCategory::Conversation);
    return Result::success();
}

Result HistoryService::addTask(const TaskRecord &record)
{
    Result guard;
    if (!writable(this, HistoryCategory::Task, &guard))
        return guard;

    QSqlQuery query(m_db);
    query.prepare(QStringLiteral(
        "INSERT INTO task_history (created_at, command_id, parameters, status, "
        "result_summary) VALUES (?, ?, ?, ?, ?)"));

    const QDateTime created = record.createdAt.isValid()
        ? record.createdAt : QDateTime::currentDateTime();

    query.addBindValue(created.toString(Qt::ISODate));
    query.addBindValue(record.commandId);
    query.addBindValue(record.parameters);
    query.addBindValue(record.status);
    query.addBindValue(record.resultSummary);

    if (!query.exec()) {
        m_lastError = query.lastError().text();
        return Result::failure(erc::HistoryQueryFailed,
                               QStringLiteral("保存任务记录失败。"), m_lastError);
    }

    emit historyChanged(HistoryCategory::Task);
    return Result::success();
}

Result HistoryService::addTranslation(const TranslationRecord &record)
{
    Result guard;
    if (!writable(this, HistoryCategory::Translation, &guard))
        return guard;

    QSqlQuery query(m_db);
    query.prepare(QStringLiteral(
        "INSERT INTO translation_history (created_at, source_lang, target_lang, "
        "source_text, translated_text, provider_id) VALUES (?, ?, ?, ?, ?, ?)"));

    const QDateTime created = record.createdAt.isValid()
        ? record.createdAt : QDateTime::currentDateTime();

    query.addBindValue(created.toString(Qt::ISODate));
    query.addBindValue(record.sourceLang);
    query.addBindValue(record.targetLang);
    query.addBindValue(record.sourceText);
    query.addBindValue(record.translatedText);
    query.addBindValue(record.providerId);

    if (!query.exec()) {
        m_lastError = query.lastError().text();
        return Result::failure(erc::HistoryQueryFailed,
                               QStringLiteral("保存翻译记录失败。"), m_lastError);
    }

    emit historyChanged(HistoryCategory::Translation);
    return Result::success();
}

Result HistoryService::addSearch(const SearchRecord &record)
{
    Result guard;
    if (!writable(this, HistoryCategory::Search, &guard))
        return guard;

    QSqlQuery query(m_db);
    query.prepare(QStringLiteral(
        "INSERT INTO search_history (created_at, query, provider_id) VALUES (?, ?, ?)"));

    const QDateTime created = record.createdAt.isValid()
        ? record.createdAt : QDateTime::currentDateTime();

    query.addBindValue(created.toString(Qt::ISODate));
    query.addBindValue(record.query);
    query.addBindValue(record.providerId);

    if (!query.exec()) {
        m_lastError = query.lastError().text();
        return Result::failure(erc::HistoryQueryFailed,
                               QStringLiteral("保存搜索记录失败。"), m_lastError);
    }

    emit historyChanged(HistoryCategory::Search);
    return Result::success();
}

Result HistoryService::addRecording(const RecordingRecord &record)
{
    Result guard;
    if (!writable(this, HistoryCategory::Recording, &guard))
        return guard;

    QSqlQuery query(m_db);
    query.prepare(QStringLiteral(
        "INSERT INTO recording_metadata (created_at, file_path, duration, format, "
        "deleted) VALUES (?, ?, ?, ?, ?)"));

    const QDateTime created = record.createdAt.isValid()
        ? record.createdAt : QDateTime::currentDateTime();

    query.addBindValue(created.toString(Qt::ISODate));
    query.addBindValue(record.filePath);
    query.addBindValue(record.duration);
    query.addBindValue(record.format);
    query.addBindValue(record.deleted ? 1 : 0);

    if (!query.exec()) {
        m_lastError = query.lastError().text();
        return Result::failure(erc::HistoryQueryFailed,
                               QStringLiteral("保存录音元数据失败。"), m_lastError);
    }

    emit historyChanged(HistoryCategory::Recording);
    return Result::success();
}

// ===========================================================================
//  查询
//
//  统一构造 WHERE 子句：
//    - keyword  -> 对内容列做 LIKE（转义 % 与 _，避免用户输入被当作通配符）
//    - from/to  -> created_at 范围
// ===========================================================================
namespace {

struct WhereClause {
    QString sql;
    QVariantList bindings;
};

// LIKE 转义：把用户输入里的 % _ \ 转义，防止「%」匹配全部记录
QString escapeLike(const QString &input)
{
    QString out = input;
    out.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
    out.replace(QLatin1Char('%'), QStringLiteral("\\%"));
    out.replace(QLatin1Char('_'), QStringLiteral("\\_"));
    return out;
}

WhereClause buildWhere(const HistoryQuery &query, const QStringList &keywordColumns)
{
    WhereClause wc;
    QStringList conditions;

    if (!query.keyword.trimmed().isEmpty() && !keywordColumns.isEmpty()) {
        QStringList orParts;
        for (const QString &col : keywordColumns) {
            orParts.append(QStringLiteral("%1 LIKE ? ESCAPE '\\'").arg(col));
            wc.bindings.append(QStringLiteral("%") + escapeLike(query.keyword.trimmed())
                               + QStringLiteral("%"));
        }
        conditions.append(QStringLiteral("(") + orParts.join(QStringLiteral(" OR "))
                          + QStringLiteral(")"));
    }

    if (query.from.isValid()) {
        conditions.append(QStringLiteral("created_at >= ?"));
        wc.bindings.append(query.from.toString(Qt::ISODate));
    }

    if (query.to.isValid()) {
        conditions.append(QStringLiteral("created_at <= ?"));
        wc.bindings.append(query.to.toString(Qt::ISODate));
    }

    if (!conditions.isEmpty())
        wc.sql = QStringLiteral(" WHERE ") + conditions.join(QStringLiteral(" AND "));

    return wc;
}

// 排序与分页：orderBy 与 limit/offset 由调用方给出，
// 但必须经过白名单校验，防止注入（SRS 7.3）
QString safeOrderBy(const QString &requested)
{
    static const QStringList allowed{
        QStringLiteral("created_at DESC"),
        QStringLiteral("created_at ASC"),
        QStringLiteral("id DESC"),
        QStringLiteral("id ASC"),
    };

    if (allowed.contains(requested))
        return requested;

    return QStringLiteral("created_at DESC");
}

} // namespace

QVector<ConversationRecord> HistoryService::queryConversation(
    const HistoryQuery &query) const
{
    QVector<ConversationRecord> out;
    if (!m_available)
        return out;

    const WhereClause wc = buildWhere(query, { QStringLiteral("content") });

    QSqlQuery sql(m_db);
    sql.prepare(QStringLiteral("SELECT id, created_at, role, content, language, "
                               "session_id FROM conversation")
                + wc.sql
                + QStringLiteral(" ORDER BY ") + safeOrderBy(query.orderBy)
                + QStringLiteral(" LIMIT ? OFFSET ?"));

    for (const QVariant &b : wc.bindings)
        sql.addBindValue(b);
    sql.addBindValue(qMax(1, query.limit));
    sql.addBindValue(qMax(0, query.offset));

    if (!sql.exec()) {
        DC_WARN(LogCat::History,
                QStringLiteral("查询对话历史失败：%1").arg(sql.lastError().text()));
        return out;
    }

    while (sql.next()) {
        ConversationRecord r;
        r.id = sql.value(0).toLongLong();
        r.createdAt = parseTime(sql.value(1));
        r.role = sql.value(2).toString();
        r.content = sql.value(3).toString();
        r.language = sql.value(4).toString();
        r.sessionId = sql.value(5).toString();
        out.append(r);
    }

    return out;
}

QVector<TaskRecord> HistoryService::queryTasks(const HistoryQuery &query) const
{
    QVector<TaskRecord> out;
    if (!m_available)
        return out;

    const WhereClause wc = buildWhere(query,
        { QStringLiteral("command_id"), QStringLiteral("result_summary") });

    QSqlQuery sql(m_db);
    sql.prepare(QStringLiteral("SELECT id, created_at, command_id, parameters, status, "
                               "result_summary FROM task_history")
                + wc.sql
                + QStringLiteral(" ORDER BY ") + safeOrderBy(query.orderBy)
                + QStringLiteral(" LIMIT ? OFFSET ?"));

    for (const QVariant &b : wc.bindings)
        sql.addBindValue(b);
    sql.addBindValue(qMax(1, query.limit));
    sql.addBindValue(qMax(0, query.offset));

    if (!sql.exec())
        return out;

    while (sql.next()) {
        TaskRecord r;
        r.id = sql.value(0).toLongLong();
        r.createdAt = parseTime(sql.value(1));
        r.commandId = sql.value(2).toString();
        r.parameters = sql.value(3).toString();
        r.status = sql.value(4).toString();
        r.resultSummary = sql.value(5).toString();
        out.append(r);
    }

    return out;
}

QVector<TranslationRecord> HistoryService::queryTranslations(
    const HistoryQuery &query) const
{
    QVector<TranslationRecord> out;
    if (!m_available)
        return out;

    const WhereClause wc = buildWhere(query,
        { QStringLiteral("source_text"), QStringLiteral("translated_text") });

    QSqlQuery sql(m_db);
    sql.prepare(QStringLiteral("SELECT id, created_at, source_lang, target_lang, "
                               "source_text, translated_text, provider_id "
                               "FROM translation_history")
                + wc.sql
                + QStringLiteral(" ORDER BY ") + safeOrderBy(query.orderBy)
                + QStringLiteral(" LIMIT ? OFFSET ?"));

    for (const QVariant &b : wc.bindings)
        sql.addBindValue(b);
    sql.addBindValue(qMax(1, query.limit));
    sql.addBindValue(qMax(0, query.offset));

    if (!sql.exec())
        return out;

    while (sql.next()) {
        TranslationRecord r;
        r.id = sql.value(0).toLongLong();
        r.createdAt = parseTime(sql.value(1));
        r.sourceLang = sql.value(2).toString();
        r.targetLang = sql.value(3).toString();
        r.sourceText = sql.value(4).toString();
        r.translatedText = sql.value(5).toString();
        r.providerId = sql.value(6).toString();
        out.append(r);
    }

    return out;
}

QVector<SearchRecord> HistoryService::querySearches(const HistoryQuery &query) const
{
    QVector<SearchRecord> out;
    if (!m_available)
        return out;

    const WhereClause wc = buildWhere(query, { QStringLiteral("query") });

    QSqlQuery sql(m_db);
    sql.prepare(QStringLiteral("SELECT id, created_at, query, provider_id "
                               "FROM search_history")
                + wc.sql
                + QStringLiteral(" ORDER BY ") + safeOrderBy(query.orderBy)
                + QStringLiteral(" LIMIT ? OFFSET ?"));

    for (const QVariant &b : wc.bindings)
        sql.addBindValue(b);
    sql.addBindValue(qMax(1, query.limit));
    sql.addBindValue(qMax(0, query.offset));

    if (!sql.exec())
        return out;

    while (sql.next()) {
        SearchRecord r;
        r.id = sql.value(0).toLongLong();
        r.createdAt = parseTime(sql.value(1));
        r.query = sql.value(2).toString();
        r.providerId = sql.value(3).toString();
        out.append(r);
    }

    return out;
}

QVector<RecordingRecord> HistoryService::queryRecordings(const HistoryQuery &query,
                                                        bool includeDeleted) const
{
    QVector<RecordingRecord> out;
    if (!m_available)
        return out;

    WhereClause wc = buildWhere(query, { QStringLiteral("file_path") });

    // 软删除过滤：默认只返回未删除的
    if (!includeDeleted) {
        if (wc.sql.isEmpty())
            wc.sql = QStringLiteral(" WHERE deleted = 0");
        else
            wc.sql += QStringLiteral(" AND deleted = 0");
    }

    QSqlQuery sql(m_db);
    sql.prepare(QStringLiteral("SELECT id, created_at, file_path, duration, format, "
                               "deleted FROM recording_metadata")
                + wc.sql
                + QStringLiteral(" ORDER BY ") + safeOrderBy(query.orderBy)
                + QStringLiteral(" LIMIT ? OFFSET ?"));

    for (const QVariant &b : wc.bindings)
        sql.addBindValue(b);
    sql.addBindValue(qMax(1, query.limit));
    sql.addBindValue(qMax(0, query.offset));

    if (!sql.exec())
        return out;

    while (sql.next()) {
        RecordingRecord r;
        r.id = sql.value(0).toLongLong();
        r.createdAt = parseTime(sql.value(1));
        r.filePath = sql.value(2).toString();
        r.duration = sql.value(3).toInt();
        r.format = sql.value(4).toString();
        r.deleted = sql.value(5).toInt() != 0;
        out.append(r);
    }

    return out;
}

QVector<HistoryEntry> HistoryService::searchAll(const HistoryQuery &query) const
{
    QVector<HistoryEntry> out;
    if (!m_available)
        return out;

    // 每张表各取一部分，最后统一按时间排序。
    // limit 分摊到各类别，避免某一类占满结果。
    HistoryQuery perTable = query;
    perTable.limit = qMax(5, query.limit);

    if (isCategoryEnabled(HistoryCategory::Conversation)) {
        for (const ConversationRecord &r : queryConversation(perTable)) {
            HistoryEntry e;
            e.category = HistoryCategory::Conversation;
            e.id = r.id;
            e.createdAt = r.createdAt;
            e.title = r.content.left(80);
            e.subtitle = QStringLiteral("%1 · %2").arg(r.role, r.language);
            e.payload = r.toJson();
            out.append(e);
        }
    }

    if (isCategoryEnabled(HistoryCategory::Task)) {
        for (const TaskRecord &r : queryTasks(perTable)) {
            HistoryEntry e;
            e.category = HistoryCategory::Task;
            e.id = r.id;
            e.createdAt = r.createdAt;
            e.title = r.commandId;
            e.subtitle = QStringLiteral("%1 · %2")
                             .arg(r.status,
                                  r.resultSummary.left(60));
            e.payload = r.toJson();
            out.append(e);
        }
    }

    if (isCategoryEnabled(HistoryCategory::Translation)) {
        for (const TranslationRecord &r : queryTranslations(perTable)) {
            HistoryEntry e;
            e.category = HistoryCategory::Translation;
            e.id = r.id;
            e.createdAt = r.createdAt;
            e.title = r.sourceText.left(60);
            e.subtitle = QStringLiteral("%1 → %2 · %3")
                             .arg(r.sourceLang, r.targetLang,
                                  r.translatedText.left(40));
            e.payload = r.toJson();
            out.append(e);
        }
    }

    if (isCategoryEnabled(HistoryCategory::Search)) {
        for (const SearchRecord &r : querySearches(perTable)) {
            HistoryEntry e;
            e.category = HistoryCategory::Search;
            e.id = r.id;
            e.createdAt = r.createdAt;
            e.title = r.query;
            e.subtitle = r.providerId;
            e.payload = r.toJson();
            out.append(e);
        }
    }

    if (isCategoryEnabled(HistoryCategory::Recording)) {
        for (const RecordingRecord &r : queryRecordings(perTable)) {
            HistoryEntry e;
            e.category = HistoryCategory::Recording;
            e.id = r.id;
            e.createdAt = r.createdAt;
            e.title = QFileInfo(r.filePath).fileName();
            e.subtitle = QStringLiteral("%1 秒 · %2").arg(r.duration).arg(r.format);
            e.payload = r.toJson();
            out.append(e);
        }
    }

    // 按时间倒序统一排序（检索结果应按最新在前展示）
    std::sort(out.begin(), out.end(), [](const HistoryEntry &a, const HistoryEntry &b) {
        return a.createdAt > b.createdAt;
    });

    if (out.size() > query.limit)
        out.resize(query.limit);

    return out;
}

int HistoryService::count(HistoryCategory category) const
{
    if (!m_available)
        return 0;

    const QString table = historyCategoryTableName(category);

    // 表名来自枚举映射（代码内固定值），不是用户输入，因此可安全拼接；
    // 但仍做一次白名单确认，避免未来扩展时引入风险。
    static const QStringList allowedTables{
        QStringLiteral("conversation"), QStringLiteral("task_history"),
        QStringLiteral("translation_history"), QStringLiteral("search_history"),
        QStringLiteral("recording_metadata")
    };
    if (!allowedTables.contains(table))
        return 0;

    QString sql = QStringLiteral("SELECT COUNT(*) FROM ") + table;
    if (category == HistoryCategory::Recording)
        sql += QStringLiteral(" WHERE deleted = 0");

    QSqlQuery query(m_db);
    if (!query.exec(sql) || !query.next())
        return 0;

    return query.value(0).toInt();
}

// ===========================================================================
//  删除
// ===========================================================================
Result HistoryService::removeWhere(HistoryCategory category,
                                  const QString &whereClause,
                                  const QVariantList &bindings)
{
    if (!m_available) {
        return Result::failure(erc::HistoryDbOpenFailed,
                               QStringLiteral("历史数据库不可用。"));
    }

    const QString table = historyCategoryTableName(category);

    static const QStringList allowedTables{
        QStringLiteral("conversation"), QStringLiteral("task_history"),
        QStringLiteral("translation_history"), QStringLiteral("search_history"),
        QStringLiteral("recording_metadata")
    };
    if (!allowedTables.contains(table)) {
        return Result::failure(erc::HistoryDeleteFailed,
                               QStringLiteral("未知的历史类别。"));
    }

    QSqlQuery query(m_db);
    query.prepare(QStringLiteral("DELETE FROM ") + table + whereClause);

    for (const QVariant &b : bindings)
        query.addBindValue(b);

    if (!query.exec()) {
        m_lastError = query.lastError().text();
        return Result::failure(erc::HistoryDeleteFailed,
                               QStringLiteral("删除历史记录失败。"), m_lastError);
    }

    emit historyChanged(category);
    return Result::success();
}

Result HistoryService::removeById(HistoryCategory category, qint64 id)
{
    return removeWhere(category, QStringLiteral(" WHERE id = ?"), { id });
}

Result HistoryService::removeByTimeRange(HistoryCategory category,
                                        const QDateTime &from, const QDateTime &to)
{
    QStringList conditions;
    QVariantList bindings;

    if (from.isValid()) {
        conditions.append(QStringLiteral("created_at >= ?"));
        bindings.append(from.toString(Qt::ISODate));
    }
    if (to.isValid()) {
        conditions.append(QStringLiteral("created_at <= ?"));
        bindings.append(to.toString(Qt::ISODate));
    }

    // 两个时间都无效时不做任何事，避免误删全部
    if (conditions.isEmpty()) {
        return Result::failure(erc::InvalidArgument,
                               QStringLiteral("未指定时间范围，为避免误删未执行任何操作。"));
    }

    return removeWhere(category,
                       QStringLiteral(" WHERE ") + conditions.join(QStringLiteral(" AND ")),
                       bindings);
}

Result HistoryService::removeAll(HistoryCategory category)
{
    return removeWhere(category, QString(), {});
}

Result HistoryService::removeEverything()
{
    static const QVector<HistoryCategory> categories{
        HistoryCategory::Conversation,
        HistoryCategory::Task,
        HistoryCategory::Translation,
        HistoryCategory::Search,
        HistoryCategory::Recording,
    };

    QStringList errors;
    for (HistoryCategory category : categories) {
        const Result r = removeAll(category);
        if (!r.ok)
            errors.append(r.message);
    }

    if (!errors.isEmpty()) {
        return Result::failure(erc::HistoryDeleteFailed,
                               QStringLiteral("部分历史记录删除失败。"),
                               errors.join(QStringLiteral("; ")));
    }

    DC_INFO(LogCat::History, QStringLiteral("已清空全部历史记录"));
    return Result::success();
}

Result HistoryService::markRecordingDeleted(qint64 id, bool deleted)
{
    if (!m_available) {
        return Result::failure(erc::HistoryDbOpenFailed,
                               QStringLiteral("历史数据库不可用。"));
    }

    QSqlQuery query(m_db);
    query.prepare(QStringLiteral(
        "UPDATE recording_metadata SET deleted = ? WHERE id = ?"));
    query.addBindValue(deleted ? 1 : 0);
    query.addBindValue(id);

    if (!query.exec()) {
        m_lastError = query.lastError().text();
        return Result::failure(erc::HistoryDeleteFailed,
                               QStringLiteral("更新录音标记失败。"), m_lastError);
    }

    emit historyChanged(HistoryCategory::Recording);
    return Result::success();
}

Result HistoryService::deleteRecordingFile(qint64 id)
{
    if (!m_available) {
        return Result::failure(erc::HistoryDbOpenFailed,
                               QStringLiteral("历史数据库不可用。"));
    }

    // 先查出文件路径
    QSqlQuery query(m_db);
    query.prepare(QStringLiteral(
        "SELECT file_path FROM recording_metadata WHERE id = ?"));
    query.addBindValue(id);

    if (!query.exec() || !query.next()) {
        return Result::failure(erc::HistoryDeleteFailed,
                               QStringLiteral("未找到该录音记录。"));
    }

    const QString filePath = query.value(0).toString();

    // SRS 4.11：录音文件默认保存在本地，删除应同时删文件与元数据。
    // 这里的删除对象是「用户主动创建的录音文件」，不是用户未知的文件，
    // 且路径来自数据库（由本程序写入），因此删除是安全的。
    if (!filePath.isEmpty() && QFile::exists(filePath)) {
        // 安全检查：只删除位于录音目录内的文件，避免数据库被篡改后
        // 导致删除任意文件（纵深防御）
        if (!m_recordingsDir.isEmpty()) {
            const QString canonicalFile = QFileInfo(filePath).canonicalFilePath();
            const QString canonicalDir = QFileInfo(m_recordingsDir).canonicalFilePath();

            if (!canonicalDir.isEmpty()
                && !canonicalFile.startsWith(canonicalDir + QLatin1Char('/'))) {
                DC_WARN(LogCat::Recording,
                        QStringLiteral("拒绝删除录音目录之外的文件：%1")
                            .arg(canonicalFile));
                return Result::failure(
                    erc::PathNotAllowed,
                    QStringLiteral("该录音文件不在录音目录内，出于安全考虑未删除文件。"));
            }
        }

        if (!QFile::remove(filePath)) {
            return Result::failure(erc::HistoryDeleteFailed,
                                   QStringLiteral("录音文件删除失败，可能正被占用。"));
        }
    }

    // 删除元数据记录
    return removeById(HistoryCategory::Recording, id);
}

// ===========================================================================
//  维护
// ===========================================================================
CleanupResult HistoryService::applyRetentionPolicy()
{
    CleanupResult result;

    if (!m_available)
        return result;

    const int retentionDays = m_privacy.retentionDays;

    // <= 0 表示永久保留（SRS 4.10：提供保留期限设置）
    if (retentionDays <= 0) {
        DC_DEBUG(LogCat::History,
                 QStringLiteral("保留期限设置为永久，跳过过期清理。"));
        return result;
    }

    const QDateTime deadline =
        QDateTime::currentDateTime().addDays(-retentionDays);

    DC_INFO(LogCat::History,
            QStringLiteral("开始清理 %1 天前的历史记录（早于 %2）")
                .arg(retentionDays)
                .arg(deadline.toString(Qt::ISODate)));

    // 先统计并删除录音文件（需要先拿到路径）
    {
        HistoryQuery q;
        q.limit = 100000;
        q.to = deadline;
        const QVector<RecordingRecord> recordings = queryRecordings(q, false);

        for (const RecordingRecord &rec : recordings) {
            if (!rec.filePath.isEmpty() && QFile::exists(rec.filePath)) {
                if (QFile::remove(rec.filePath)) {
                    ++result.recordingFilesRemoved;
                } else {
                    result.errors.append(
                        QStringLiteral("无法删除录音文件：%1").arg(rec.filePath));
                }
            }
        }
    }

    static const QVector<QPair<HistoryCategory, int CleanupResult::*>> mapping{
        { HistoryCategory::Conversation, &CleanupResult::conversationRemoved },
        { HistoryCategory::Task,         &CleanupResult::taskRemoved },
        { HistoryCategory::Translation,  &CleanupResult::translationRemoved },
        { HistoryCategory::Search,       &CleanupResult::searchRemoved },
        { HistoryCategory::Recording,    &CleanupResult::recordingRemoved },
    };

    const QString deadlineStr = deadline.toString(Qt::ISODate);

    for (const auto &entry : mapping) {
        const QString table = historyCategoryTableName(entry.first);

        // 先计数
        int before = count(entry.first);

        const Result r = removeWhere(entry.first,
                                     QStringLiteral(" WHERE created_at < ?"),
                                     { deadlineStr });
        if (!r.ok) {
            result.errors.append(r.message);
            continue;
        }

        const int after = count(entry.first);
        result.*(entry.second) = qMax(0, before - after);
        Q_UNUSED(table)
    }

    if (result.totalRemoved() > 0) {
        // 删除后回收空间
        compact();

        DC_INFO(LogCat::History,
                QStringLiteral("保留策略清理完成：共删除 %1 条记录、%2 个录音文件")
                    .arg(result.totalRemoved())
                    .arg(result.recordingFilesRemoved));
    }

    return result;
}

CleanupResult HistoryService::enforceStorageLimit()
{
    CleanupResult result;

    if (!m_available || m_privacy.maxStorageBytes <= 0)
        return result;

    HistoryStorageStats stats = storageStats();

    if (stats.totalBytes <= m_privacy.maxStorageBytes) {
        DC_DEBUG(LogCat::History,
                 QStringLiteral("存储占用 %1 MB，未超过上限 %2 MB")
                     .arg(stats.totalBytes / (1024 * 1024))
                     .arg(m_privacy.maxStorageBytes / (1024 * 1024)));
        return result;
    }

    DC_WARN(LogCat::History,
            QStringLiteral("存储占用 %1 MB 超过上限 %2 MB，开始按最旧优先清理。")
                .arg(stats.totalBytes / (1024 * 1024))
                .arg(m_privacy.maxStorageBytes / (1024 * 1024)));

    // 逐类从最旧记录开始删除，直到降到上限的 80%（留出余量，
    // 避免每次写入都触发清理）
    const qint64 targetBytes = static_cast<qint64>(m_privacy.maxStorageBytes * 0.8);

    static const QVector<HistoryCategory> categories{
        HistoryCategory::Conversation,
        HistoryCategory::Task,
        HistoryCategory::Translation,
        HistoryCategory::Search,
        HistoryCategory::Recording,
    };

    // 多轮清理：每轮删除各类别最旧的一批
    for (int round = 0; round < 50; ++round) {
        stats = storageStats();
        if (stats.totalBytes <= targetBytes)
            break;

        bool removedAny = false;

        for (HistoryCategory category : categories) {
            // 取最旧的 20 条
            HistoryQuery q;
            q.limit = 20;
            q.orderBy = QStringLiteral("created_at ASC");

            QVector<qint64> ids;
            switch (category) {
            case HistoryCategory::Conversation:
                for (const auto &r : queryConversation(q)) ids.append(r.id);
                break;
            case HistoryCategory::Task:
                for (const auto &r : queryTasks(q)) ids.append(r.id);
                break;
            case HistoryCategory::Translation:
                for (const auto &r : queryTranslations(q)) ids.append(r.id);
                break;
            case HistoryCategory::Search:
                for (const auto &r : querySearches(q)) ids.append(r.id);
                break;
            case HistoryCategory::Recording:
                for (const auto &r : queryRecordings(q, true)) ids.append(r.id);
                break;
            }

            for (qint64 id : ids) {
                if (category == HistoryCategory::Recording) {
                    const Result r = deleteRecordingFile(id);
                    if (r.ok)
                        ++result.recordingRemoved;
                } else {
                    const Result r = removeById(category, id);
                    if (!r.ok)
                        continue;

                    switch (category) {
                    case HistoryCategory::Conversation: ++result.conversationRemoved; break;
                    case HistoryCategory::Task:         ++result.taskRemoved; break;
                    case HistoryCategory::Translation:  ++result.translationRemoved; break;
                    case HistoryCategory::Search:       ++result.searchRemoved; break;
                    default: break;
                    }
                }
                removedAny = true;
            }
        }

        if (!removedAny)
            break;   // 已经删无可删，但还没降到目标（可能是录音文件占用）
    }

    compact();

    const HistoryStorageStats after = storageStats();
    result.bytesFreed = qMax(0, stats.totalBytes - after.totalBytes);

    DC_INFO(LogCat::History,
            QStringLiteral("存储上限清理完成：删除 %1 条记录，释放约 %2 MB")
                .arg(result.totalRemoved())
                .arg(result.bytesFreed / (1024 * 1024)));

    return result;
}

Result HistoryService::compact()
{
    if (!m_available) {
        return Result::failure(erc::HistoryDbOpenFailed,
                               QStringLiteral("历史数据库不可用。"));
    }

    // VACUUM 需要在没有活动事务时执行
    QString error;
    if (!execSimple(QStringLiteral("VACUUM"), &error)) {
        return Result::failure(erc::HistoryQueryFailed,
                               QStringLiteral("数据库压缩失败。"), error);
    }
    execSimple(QStringLiteral("PRAGMA optimize"));

    return Result::success();
}

HistoryStorageStats HistoryService::storageStats() const
{
    HistoryStorageStats stats;
    stats.maxStorageBytes = m_privacy.maxStorageBytes;

    if (!m_available)
        return stats;

    stats.conversationCount = count(HistoryCategory::Conversation);
    stats.taskCount = count(HistoryCategory::Task);
    stats.translationCount = count(HistoryCategory::Translation);
    stats.searchCount = count(HistoryCategory::Search);
    stats.recordingCount = count(HistoryCategory::Recording);

    // 数据库大小（含 WAL 与 SHM 文件）
    stats.databaseBytes = QFileInfo(m_databasePath).size();
    for (const QString &suffix : { QStringLiteral("-wal"), QStringLiteral("-shm") }) {
        const QFileInfo fi(m_databasePath + suffix);
        if (fi.exists())
            stats.databaseBytes += fi.size();
    }

    // 录音文件大小（按元数据累计，避免遍历目录）
    {
        QSqlQuery query(m_db);
        if (query.exec(QStringLiteral(
                "SELECT file_path FROM recording_metadata WHERE deleted = 0"))) {
            while (query.next()) {
                const QFileInfo fi(query.value(0).toString());
                if (fi.exists())
                    stats.recordingsBytes += fi.size();
            }
        }
    }

    stats.totalBytes = stats.databaseBytes + stats.recordingsBytes;

    if (stats.maxStorageBytes > 0)
        stats.usageRatio = static_cast<double>(stats.totalBytes)
                           / static_cast<double>(stats.maxStorageBytes);

    return stats;
}

qint64 HistoryService::directorySize(const QString &path)
{
    qint64 total = 0;
    QDirIterator it(path, QDir::Files | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        total += it.fileInfo().size();
    }
    return total;
}

// ===========================================================================
//  导出
// ===========================================================================
Result HistoryService::exportAll(const QString &path, const QString &format) const
{
    if (!m_available) {
        return Result::failure(erc::HistoryDbOpenFailed,
                               QStringLiteral("历史数据库不可用，无法导出。"));
    }

    HistoryQuery query;
    query.limit = 1000000;   // 导出不设实际上限

    if (format.compare(QStringLiteral("csv"), Qt::CaseInsensitive) == 0) {
        QString csv;
        csv += QStringLiteral("category,id,created_at,summary,detail\n");

        auto appendCsvRow = [&csv](const QString &category, qint64 id,
                                  const QDateTime &created,
                                  const QString &summary, const QString &detail) {
            auto esc = [](QString s) {
                s.replace(QLatin1Char('"'), QStringLiteral("\"\""));
                return QStringLiteral("\"") + s + QStringLiteral("\"");
            };
            csv += QStringLiteral("%1,%2,%3,%4,%5\n")
                       .arg(esc(category))
                       .arg(id)
                       .arg(esc(created.toString(Qt::ISODate)))
                       .arg(esc(summary))
                       .arg(esc(detail));
        };

        for (const HistoryEntry &e : searchAll(query)) {
            appendCsvRow(historyCategoryToString(e.category), e.id,
                         e.createdAt, e.title, e.subtitle);
        }

        QDir().mkpath(QFileInfo(path).absolutePath());
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            return Result::failure(erc::HistoryExportFailed,
                                   QStringLiteral("无法写入导出文件：%1")
                                       .arg(file.errorString()));
        }
        // CSV 用 UTF-8 BOM 开头，便于 Excel 正确识别中文
        file.write("\xEF\xBB\xBF");
        file.write(csv.toUtf8());
        file.close();

        DC_INFO(LogCat::History, QStringLiteral("已导出历史记录（CSV）：%1").arg(path));
        return Result::success();
    }

    // ---- JSON ----
    QJsonObject root;
    root[QStringLiteral("exported_at")] =
        QDateTime::currentDateTime().toString(Qt::ISODate);
    root[QStringLiteral("app_version")] = QString::fromLatin1(dc::buildinfo::kAppVersion);

    // 说明：导出内容包含用户自己的历史数据，属于用户主动请求的操作。
    //      本程序不会自动上传，也不在导出中附加任何其他信息。
    root[QStringLiteral("note")] = QStringLiteral(
        "本文件包含你在本机保存的历史记录。请妥善保管，避免分享给他人。");

    QJsonArray convArr, taskArr, transArr, searchArr, recArr;

    for (const ConversationRecord &r : queryConversation(query))
        convArr.append(r.toJson());
    for (const TaskRecord &r : queryTasks(query))
        taskArr.append(r.toJson());
    for (const TranslationRecord &r : queryTranslations(query))
        transArr.append(r.toJson());
    for (const SearchRecord &r : querySearches(query))
        searchArr.append(r.toJson());
    for (const RecordingRecord &r : queryRecordings(query, true))
        recArr.append(r.toJson());

    root[QStringLiteral("conversation")] = convArr;
    root[QStringLiteral("task_history")] = taskArr;
    root[QStringLiteral("translation_history")] = transArr;
    root[QStringLiteral("search_history")] = searchArr;
    root[QStringLiteral("recording_metadata")] = recArr;

    QDir().mkpath(QFileInfo(path).absolutePath());

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return Result::failure(erc::HistoryExportFailed,
                               QStringLiteral("无法写入导出文件：%1")
                                   .arg(file.errorString()));
    }

    file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    file.close();

    DC_INFO(LogCat::History, QStringLiteral("已导出历史记录（JSON）：%1").arg(path));
    return Result::success();
}

Result HistoryService::exportCategory(HistoryCategory category,
                                     const QString &path) const
{
    if (!m_available) {
        return Result::failure(erc::HistoryDbOpenFailed,
                               QStringLiteral("历史数据库不可用，无法导出。"));
    }

    HistoryQuery query;
    query.limit = 1000000;

    QJsonArray arr;

    switch (category) {
    case HistoryCategory::Conversation:
        for (const ConversationRecord &r : queryConversation(query))
            arr.append(r.toJson());
        break;
    case HistoryCategory::Task:
        for (const TaskRecord &r : queryTasks(query))
            arr.append(r.toJson());
        break;
    case HistoryCategory::Translation:
        for (const TranslationRecord &r : queryTranslations(query))
            arr.append(r.toJson());
        break;
    case HistoryCategory::Search:
        for (const SearchRecord &r : querySearches(query))
            arr.append(r.toJson());
        break;
    case HistoryCategory::Recording:
        for (const RecordingRecord &r : queryRecordings(query, true))
            arr.append(r.toJson());
        break;
    }

    QJsonObject root;
    root[QStringLiteral("category")] = historyCategoryToString(category);
    root[QStringLiteral("exported_at")] =
        QDateTime::currentDateTime().toString(Qt::ISODate);
    root[QStringLiteral("count")] = arr.size();
    root[QStringLiteral("records")] = arr;

    QDir().mkpath(QFileInfo(path).absolutePath());

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return Result::failure(erc::HistoryExportFailed,
                               QStringLiteral("无法写入导出文件：%1")
                                   .arg(file.errorString()));
    }

    file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    file.close();

    return Result::success();
}

// ===========================================================================
//  完整性自检与恢复（SRS 7.2）
// ===========================================================================
bool HistoryService::verifyIntegrity(QString *detail)
{
    if (!m_db.isOpen()) {
        if (detail)
            *detail = QStringLiteral("数据库未打开。");
        return false;
    }

    QSqlQuery query(m_db);
    if (!query.exec(QStringLiteral("PRAGMA integrity_check"))) {
        if (detail)
            *detail = query.lastError().text();
        return false;
    }

    if (!query.next()) {
        if (detail)
            *detail = QStringLiteral("完整性检查没有返回结果。");
        return false;
    }

    const QString result = query.value(0).toString();

    if (result.compare(QStringLiteral("ok"), Qt::CaseInsensitive) == 0) {
        if (detail)
            *detail = QStringLiteral("ok");
        return true;
    }

    // ---- 数据库损坏：备份 + 重建 ----
    DC_ERROR(LogCat::History,
             QStringLiteral("历史数据库完整性检查失败：%1").arg(result));

    if (detail)
        *detail = result;

    const QString backup = m_databasePath + QStringLiteral(".corrupted.")
        + QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss"));

    // 关闭 -> 备份 -> 删除 -> 重建
    m_db.close();

    if (QFile::exists(m_databasePath))
        QFile::copy(m_databasePath, backup);

    for (const QString &suffix : { QString(), QStringLiteral("-wal"),
                                   QStringLiteral("-shm") }) {
        QFile::remove(m_databasePath + suffix);
    }

    if (!m_db.open()) {
        DC_ERROR(LogCat::History,
                 QStringLiteral("重建历史数据库失败：%1").arg(m_db.lastError().text()));
        return false;
    }

    if (!createSchema()) {
        DC_ERROR(LogCat::History, QStringLiteral("重建历史数据库表结构失败。"));
        return false;
    }

    DC_WARN(LogCat::History,
            QStringLiteral("历史数据库已重建，原文件备份至：%1").arg(backup));

    // 在事件中告知用户（SRS 7.2：应有可理解的错误处理）
    emit storageWarning(
        QStringLiteral("历史数据库检测到损坏，已备份原文件并重建。"
                       "原数据文件位于：%1").arg(backup));

    if (detail)
        *detail = QStringLiteral("已损坏并重建，备份：%1").arg(backup);

    return false;
}

} // namespace dc
