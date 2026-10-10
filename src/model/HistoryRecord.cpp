// ============================================================================
//  HistoryRecord.cpp
// ============================================================================
#include "model/HistoryRecord.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>

namespace dc {

namespace {

// 统一的 ISO 时间序列化，保证 SQLite 中按字符串排序即为按时间排序
QString iso(const QDateTime &dt)
{
    return dt.isValid() ? dt.toString(Qt::ISODate) : QString();
}

} // namespace

// ---------------------------------------------------------------------------
QJsonObject ConversationRecord::toJson() const
{
    QJsonObject obj;
    obj[QStringLiteral("id")] = id;
    obj[QStringLiteral("created_at")] = iso(createdAt);
    obj[QStringLiteral("role")] = role;
    obj[QStringLiteral("content")] = content;
    obj[QStringLiteral("language")] = language;
    obj[QStringLiteral("session_id")] = sessionId;
    return obj;
}

QJsonObject TaskRecord::toJson() const
{
    QJsonObject obj;
    obj[QStringLiteral("id")] = id;
    obj[QStringLiteral("created_at")] = iso(createdAt);
    obj[QStringLiteral("command_id")] = commandId;
    // parameters 本身就是 JSON 字符串，这里嵌入为字符串以保持与表结构一致；
    // 若可解析则嵌入为对象，便于导出后直接阅读。
    const QJsonDocument paramDoc = QJsonDocument::fromJson(parameters.toUtf8());
    obj[QStringLiteral("parameters")] =
        paramDoc.isNull() ? QJsonValue(parameters) : QJsonValue(paramDoc.object());
    obj[QStringLiteral("status")] = status;
    obj[QStringLiteral("result_summary")] = resultSummary;
    return obj;
}

QJsonObject TranslationRecord::toJson() const
{
    QJsonObject obj;
    obj[QStringLiteral("id")] = id;
    obj[QStringLiteral("created_at")] = iso(createdAt);
    obj[QStringLiteral("source_lang")] = sourceLang;
    obj[QStringLiteral("target_lang")] = targetLang;
    obj[QStringLiteral("source_text")] = sourceText;
    obj[QStringLiteral("translated_text")] = translatedText;
    obj[QStringLiteral("provider_id")] = providerId;
    return obj;
}

QJsonObject SearchRecord::toJson() const
{
    QJsonObject obj;
    obj[QStringLiteral("id")] = id;
    obj[QStringLiteral("created_at")] = iso(createdAt);
    obj[QStringLiteral("query")] = query;
    obj[QStringLiteral("provider_id")] = providerId;
    return obj;
}

QJsonObject RecordingRecord::toJson() const
{
    QJsonObject obj;
    obj[QStringLiteral("id")] = id;
    obj[QStringLiteral("created_at")] = iso(createdAt);
    obj[QStringLiteral("file_path")] = filePath;
    obj[QStringLiteral("duration")] = duration;
    obj[QStringLiteral("format")] = format;
    obj[QStringLiteral("deleted")] = deleted ? 1 : 0;
    return obj;
}

QJsonObject HistoryEntry::toJson() const
{
    QJsonObject obj;
    obj[QStringLiteral("category")] = historyCategoryToString(category);
    obj[QStringLiteral("id")] = id;
    obj[QStringLiteral("created_at")] = iso(createdAt);
    obj[QStringLiteral("title")] = title;
    obj[QStringLiteral("subtitle")] = subtitle;
    obj[QStringLiteral("payload")] = payload;
    return obj;
}

// ---------------------------------------------------------------------------
bool PrivacySettings::isCategoryEnabled(HistoryCategory category) const
{
    switch (category) {
    case HistoryCategory::Conversation: return conversationEnabled;
    case HistoryCategory::Task:         return taskEnabled;
    case HistoryCategory::Translation:  return translationEnabled;
    case HistoryCategory::Search:       return searchEnabled;
    case HistoryCategory::Recording:    return recordingEnabled;
    }
    return false;   // 未知类别默认不记录（隐私优先）
}

} // namespace dc
