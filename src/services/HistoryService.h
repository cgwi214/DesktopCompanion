// ============================================================================
//  HistoryService.h — 本地历史记录（SQLite）
//
//  需求依据：SRS V1.1 第 4.10 节（历史记录与隐私）
//    - 本地存储优先使用 SQLite
//    - 五张表：conversation / task_history / translation_history /
//              search_history / recording_metadata
//    - 聊天、翻译、搜索和任务历史均可分别启用或关闭
//    - 提供按条删除、按时间范围删除、全部清空和导出功能
//    - 提供保留期限与最大占用空间设置
//    - 不得在未经用户明确许可时上传录音或历史记录
//    - 程序异常退出后，下次启动应能够恢复基本运行，
//      不应损坏历史数据库（SRS 7.2）
//
//  实现要点：
//    1. 使用 WAL 模式提升并发读性能，且在异常退出时更不容易损坏
//    2. 所有写操作在事务中完成（原子性）
//    3. 隐私开关在「写入前」判定 —— 关闭的功能绝不产生数据，
//       而不是先写入再过滤（后者会留下痕迹）
//    4. OPEN_READWRITE | OPEN_CREATE 失败时自动重建数据库并备份原文件
// ============================================================================
#pragma once

#include "model/HistoryRecord.h"
#include "model/Types.h"

#include <QObject>
#include <QSqlDatabase>
#include <QString>
#include <QStringList>
#include <QDateTime>
#include <QVector>

namespace dc {

// ---------------------------------------------------------------------------
// 存储占用统计
// ---------------------------------------------------------------------------
struct HistoryStorageStats {
    qint64 databaseBytes = 0;        // 数据库文件大小
    qint64 recordingsBytes = 0;      // 录音文件总大小
    qint64 totalBytes = 0;
    int conversationCount = 0;
    int taskCount = 0;
    int translationCount = 0;
    int searchCount = 0;
    int recordingCount = 0;
    qint64 maxStorageBytes = 0;      // 配置的上限
    double usageRatio = 0.0;         // totalBytes / maxStorageBytes
};

// ---------------------------------------------------------------------------
// 清理结果
// ---------------------------------------------------------------------------
struct CleanupResult {
    int conversationRemoved = 0;
    int taskRemoved = 0;
    int translationRemoved = 0;
    int searchRemoved = 0;
    int recordingRemoved = 0;
    int recordingFilesRemoved = 0;
    qint64 bytesFreed = 0;
    QStringList errors;

    int totalRemoved() const
    {
        return conversationRemoved + taskRemoved + translationRemoved
             + searchRemoved + recordingRemoved;
    }
};

// ---------------------------------------------------------------------------
// HistoryService
// ---------------------------------------------------------------------------
class HistoryService : public QObject
{
    Q_OBJECT

public:
    explicit HistoryService(QObject *parent = nullptr);
    ~HistoryService() override;

    // -----------------------------------------------------------------------
    // 初始化 / 关闭
    //
    //  databasePath 为空时由调用方通过 setDefaultPath() 预先设置。
    //  返回 false 表示数据库不可用 —— 此时所有写入被静默丢弃、
    //  所有读取返回空集，程序其余功能不受影响（SRS 7.2）。
    // -----------------------------------------------------------------------
    bool initialize(const QString &databasePath);
    void close();
    bool isAvailable() const;
    QString lastError() const;

    // 隐私设置（SRS 4.10：分别启用或关闭）
    void setPrivacySettings(const PrivacySettings &settings);
    PrivacySettings privacySettings() const;

    // 录音目录（用于统计与清理）
    void setRecordingsDirectory(const QString &dir);
    QString recordingsDirectory() const;

    // -----------------------------------------------------------------------
    // 写入（各自检查对应的隐私开关）
    // -----------------------------------------------------------------------
    Result addConversation(const ConversationRecord &record);
    Result addTask(const TaskRecord &record);
    Result addTranslation(const TranslationRecord &record);
    Result addSearch(const SearchRecord &record);
    Result addRecording(const RecordingRecord &record);

    // -----------------------------------------------------------------------
    // 查询（SRS 4.10：检索）
    // -----------------------------------------------------------------------
    QVector<ConversationRecord> queryConversation(const HistoryQuery &query) const;
    QVector<TaskRecord> queryTasks(const HistoryQuery &query) const;
    QVector<TranslationRecord> queryTranslations(const HistoryQuery &query) const;
    QVector<SearchRecord> querySearches(const HistoryQuery &query) const;
    QVector<RecordingRecord> queryRecordings(const HistoryQuery &query,
                                            bool includeDeleted = false) const;

    // 跨表统一检索（历史记录窗口的「全部」标签页）
    QVector<HistoryEntry> searchAll(const HistoryQuery &query) const;

    // 计数
    int count(HistoryCategory category) const;

    // -----------------------------------------------------------------------
    // 删除（SRS 4.10：按条删除、按时间范围删除、全部清空）
    // -----------------------------------------------------------------------
    Result removeById(HistoryCategory category, qint64 id);
    Result removeByTimeRange(HistoryCategory category,
                            const QDateTime &from, const QDateTime &to);
    Result removeAll(HistoryCategory category);
    Result removeEverything();

    // 录音软删除 / 恢复 / 真正删除文件
    Result markRecordingDeleted(qint64 id, bool deleted);
    Result deleteRecordingFile(qint64 id);

    // -----------------------------------------------------------------------
    // 维护（SRS 4.10：保留期限与最大占用空间）
    // -----------------------------------------------------------------------
    CleanupResult applyRetentionPolicy();
    CleanupResult enforceStorageLimit();

    // 数据库完整性维护：VACUUM + PRAGMA optimize
    Result compact();

    HistoryStorageStats storageStats() const;

    // -----------------------------------------------------------------------
    // 导出（SRS 4.10：导出功能）
    // -----------------------------------------------------------------------
    //  format: "json" / "csv"
    Result exportAll(const QString &path, const QString &format = QStringLiteral("json")) const;
    Result exportCategory(HistoryCategory category, const QString &path) const;

    // -----------------------------------------------------------------------
    // 数据库自检与恢复（SRS 7.2：异常退出后不应损坏数据库）
    // -----------------------------------------------------------------------
    //  返回 true 表示数据库健康；false 表示已执行恢复（重建）操作
    bool verifyIntegrity(QString *detail = nullptr);

signals:
    void historyChanged(dc::HistoryCategory category);
    void storageWarning(const QString &message);

private:
    // 建表与迁移
    bool createSchema();
    bool applyMigrations(int fromVersion);
    int currentSchemaVersion() const;

    // 表是否存在
    bool tableExists(const QString &tableName) const;
    bool execSimple(const QString &sql, QString *error = nullptr) const;

    // 通用删除辅助
    Result removeWhere(HistoryCategory category, const QString &whereClause,
                      const QVariantList &bindings);

    // 隐私开关检查
    bool isCategoryEnabled(HistoryCategory category) const;
    Result privacyDisabledResult(HistoryCategory category) const;

    // 计算目录大小
    static qint64 directorySize(const QString &path);

    QSqlDatabase m_db;
    QString m_connectionName;
    QString m_databasePath;
    QString m_recordingsDir;
    QString m_lastError;

    bool m_available = false;
    PrivacySettings m_privacy;

    static constexpr const char *kSchemaVersionKey = "schema_version";
};

} // namespace dc
