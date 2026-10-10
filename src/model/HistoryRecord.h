// ============================================================================
//  HistoryRecord.h — 历史记录数据结构
//
//  需求依据：SRS V1.1 第 4.10 节（历史记录与隐私）
//    数据库表：conversation / task_history / translation_history /
//              search_history / recording_metadata
//
//  设计说明：
//    每个表对应一个 struct，字段与 SRS 表格逐列对齐，
//    便于在 docs/MODULE_SPEC.md 中做「需求字段 <-> 代码字段」对照。
//
//  隐私约束（SRS 4.10）：
//    - 五类历史可分别启用/关闭  -> retrievalEnabled 在 HistoryService 中按类别判断
//    - 支持按条/按时间范围删除、清空、导出
//    - 提供保留期限与最大占用空间设置
// ============================================================================
#pragma once

#include "model/Types.h"

#include <QDateTime>
#include <QJsonObject>
#include <QString>
#include <QVector>

namespace dc {

// ---------------------------------------------------------------------------
// conversation 表
// ---------------------------------------------------------------------------
struct ConversationRecord {
    qint64 id = -1;
    QDateTime createdAt;
    QString role;        // user / assistant / system
    QString content;
    QString language;    // 语言标识
    QString sessionId;

    static QString tableName() { return QStringLiteral("conversation"); }
    QJsonObject toJson() const;
};

// ---------------------------------------------------------------------------
// task_history 表
// ---------------------------------------------------------------------------
struct TaskRecord {
    qint64 id = -1;
    QDateTime createdAt;
    QString commandId;
    QString parameters;      // 参数 JSON 字符串
    QString status;          // 成功、失败、取消等
    QString resultSummary;

    static QString tableName() { return QStringLiteral("task_history"); }
    QJsonObject toJson() const;
};

// ---------------------------------------------------------------------------
// translation_history 表
// ---------------------------------------------------------------------------
struct TranslationRecord {
    qint64 id = -1;
    QDateTime createdAt;
    QString sourceLang;
    QString targetLang;
    QString sourceText;
    QString translatedText;
    QString providerId;

    static QString tableName() { return QStringLiteral("translation_history"); }
    QJsonObject toJson() const;
};

// ---------------------------------------------------------------------------
// search_history 表
// ---------------------------------------------------------------------------
struct SearchRecord {
    qint64 id = -1;
    QDateTime createdAt;
    QString query;
    QString providerId;

    static QString tableName() { return QStringLiteral("search_history"); }
    QJsonObject toJson() const;
};

// ---------------------------------------------------------------------------
// recording_metadata 表
//   SRS 4.11：录音文件默认保存在本地；本表只记录元数据，不保存音频内容
// ---------------------------------------------------------------------------
struct RecordingRecord {
    qint64 id = -1;
    QDateTime createdAt;
    QString filePath;
    int duration = 0;        // 秒
    QString format;
    bool deleted = false;    // 删除标记（软删除，便于用户反悔）

    static QString tableName() { return QStringLiteral("recording_metadata"); }
    QJsonObject toJson() const;
};

// ---------------------------------------------------------------------------
// 检索条件（SRS 4.10：检索、按时间范围删除）
// ---------------------------------------------------------------------------
struct HistoryQuery {
    QString keyword;                 // 关键词（对内容字段做 LIKE 匹配）
    QDateTime from;                  // 起始时间（无效表示不限）
    QDateTime to;                    // 结束时间（无效表示不限）
    int limit = 100;
    int offset = 0;
    QString orderBy = QStringLiteral("created_at DESC");

    bool hasTimeRange() const { return from.isValid() || to.isValid(); }
};

// ---------------------------------------------------------------------------
// 统一的历史检索结果（用于跨表展示）
// ---------------------------------------------------------------------------
struct HistoryEntry {
    HistoryCategory category = HistoryCategory::Conversation;
    qint64 id = -1;
    QDateTime createdAt;
    QString title;      // 列表主行
    QString subtitle;   // 列表副行
    QJsonObject payload;  // 完整记录

    QJsonObject toJson() const;
};

// ---------------------------------------------------------------------------
// 隐私与容量设置（SRS 4.10）
// ---------------------------------------------------------------------------
struct PrivacySettings {
    bool conversationEnabled = true;
    bool taskEnabled = true;
    bool translationEnabled = true;
    bool searchEnabled = true;
    bool recordingEnabled = true;

    int retentionDays = 180;          // 保留期限，<=0 表示永久
    qint64 maxStorageBytes = 512LL * 1024 * 1024;  // 最大占用空间

    bool isCategoryEnabled(HistoryCategory category) const;
};

} // namespace dc
