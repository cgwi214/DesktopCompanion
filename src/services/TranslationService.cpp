#include "TranslationService.h"

#include "core/ErrorCode.h"
#include "core/Logger.h"
#include "platform/PathGuard.h"
#include "services/NetworkService.h"
#include "services/SecureStore.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QPointer>
#include <QRegularExpression>
#include <QTextStream>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>

#include <algorithm>

namespace dc {

static inline QString ec(const char *code)
{
    return QString::fromLatin1(code);
}

static qint64 nowMs()
{
    return QDateTime::currentMSecsSinceEpoch();
}

// ============================================================================
// 内置语言表
// ============================================================================
QList<LanguageOption> TranslationService::supportedLanguages()
{
    // 覆盖面按"这个项目的实际用户可能用到"来定，不做全集。
    // 每个条目都带英文名，因为多数翻译 API 用英文名做参数。
    static const QList<LanguageOption> kLanguages = {
        { QStringLiteral("zh"),      QObject::tr("中文"),     QStringLiteral("Chinese") },
        { QStringLiteral("zh-Hant"), QObject::tr("繁體中文"), QStringLiteral("Chinese (Traditional)") },
        { QStringLiteral("en"),      QObject::tr("英语"),     QStringLiteral("English") },
        { QStringLiteral("ja"),      QObject::tr("日语"),     QStringLiteral("Japanese") },
        { QStringLiteral("ko"),      QObject::tr("韩语"),     QStringLiteral("Korean") },
        { QStringLiteral("fr"),      QObject::tr("法语"),     QStringLiteral("French") },
        { QStringLiteral("de"),      QObject::tr("德语"),     QStringLiteral("German") },
        { QStringLiteral("es"),      QObject::tr("西班牙语"), QStringLiteral("Spanish") },
        { QStringLiteral("ru"),      QObject::tr("俄语"),     QStringLiteral("Russian") },
        { QStringLiteral("pt"),      QObject::tr("葡萄牙语"), QStringLiteral("Portuguese") },
        { QStringLiteral("it"),      QObject::tr("意大利语"), QStringLiteral("Italian") },
        { QStringLiteral("ar"),      QObject::tr("阿拉伯语"), QStringLiteral("Arabic") },
        { QStringLiteral("th"),      QObject::tr("泰语"),     QStringLiteral("Thai") },
        { QStringLiteral("vi"),      QObject::tr("越南语"),   QStringLiteral("Vietnamese") },
    };
    return kLanguages;
}

QString TranslationService::languageDisplayName(const QString &code)
{
    const QString normalized = normalizeLanguageCode(code);
    if (normalized.isEmpty() || normalized == QLatin1String("auto")) {
        return QObject::tr("自动检测");
    }
    const QList<LanguageOption> langs = supportedLanguages();
    for (const LanguageOption &l : langs) {
        if (normalizeLanguageCode(l.code) == normalized) {
            return l.displayName;
        }
    }
    // 未知语言：返回代码本身而不是"未知"。
    // 用户看到 "af" 至少知道系统识别成了南非荷兰语，
    // 看到"未知"则完全无从判断。
    return code;
}

QString TranslationService::normalizeLanguageCode(const QString &code)
{
    QString c = code.trimmed();
    if (c.isEmpty()) {
        return QString();
    }
    if (c.compare(QLatin1String("auto"), Qt::CaseInsensitive) == 0) {
        return QStringLiteral("auto");
    }

    // "zh-CN" / "zh_CN" -> 统一分隔符
    c.replace(QLatin1Char('_'), QLatin1Char('-'));

    // 主语言小写
    const int dash = c.indexOf(QLatin1Char('-'));
    if (dash > 0) {
        const QString main = c.left(dash).toLower();
        const QString sub = c.mid(dash + 1);
        // 地区码大写，脚本码首字母大写（zh-Hans）
        QString normalizedSub = sub.toLower();
        if (sub.length() == 2) {
            normalizedSub = sub.toUpper();
        } else if (sub.length() > 2) {
            normalizedSub = sub.left(1).toUpper() + sub.mid(1).toLower();
        }
        return main + QLatin1Char('-') + normalizedSub;
    }

    return c.toLower();
}

QString TranslationService::suggestTargetLanguage(const QString &detectedSource)
{
    const QString src = normalizeLanguageCode(detectedSource);

    // 中文 <-> 英文 是最常见的一对，其余情况默认给英文
    if (src == QLatin1String("zh") || src == QLatin1String("zh-Hant")) {
        return QStringLiteral("en");
    }
    if (src == QLatin1String("en")) {
        return QStringLiteral("zh");
    }
    if (src == QLatin1String("ja")) {
        return QStringLiteral("zh");
    }
    return QStringLiteral("en");
}

// ============================================================================
// 文本切分
//
//  这是翻译功能里最容易出问题的一环。几个必须处理的情况：
//    1. 过长的单段（比如用户粘了一整篇没有换行的文章）
//    2. 段落内的句子边界（尽量在句号/换行处切，不要从词中间切开）
//    3. 非常长的"句子"（代码、无标点的连续字符）——只能硬切
//    4. 保留原始段落结构，否则译文会变成一大坨
// ============================================================================
QStringList TranslationService::splitText(const QString &text, int maxChars,
                                          bool preserveParagraphs)
{
    QStringList result;
    if (text.isEmpty()) {
        return result;
    }
    if (maxChars <= 0) {
        maxChars = 1000;
    }

    // 第一步：按段落切。用 QRegularExpression 处理 \r\n / \n / \r 三种换行，
    // 并且保留空行信息（空行本身也是结构的一部分，翻译时保留为分隔）。
    QStringList rawParagraphs;
    if (preserveParagraphs) {
        static const QRegularExpression kParaSplit(QStringLiteral("\\r\\n|\\r|\\n"));
        rawParagraphs = text.split(kParaSplit);
    } else {
        rawParagraphs << text;
    }

    for (const QString &paragraph : rawParagraphs) {
        if (paragraph.isEmpty()) {
            // 保留空段落 —— 它是译文分段的结构信息
            result << QString();
            continue;
        }

        if (paragraph.length() <= maxChars) {
            result << paragraph;
            continue;
        }

        // 第二步：段落超长，按句子边界切
        // 断句符：中日韩句号、英文句号问号叹号、分号、逗号（作为次优选择）
        static const QRegularExpression kSentenceSplit(
            QStringLiteral("(?<=[。！？；.!?;])\\s*"));

        const QStringList sentences = paragraph.split(kSentenceSplit);

        QString current;
        for (const QString &sentence : sentences) {
            if (sentence.isEmpty()) {
                continue;
            }

            // 单句本身就超长（例如一整段没有标点的代码）
            if (sentence.length() > maxChars) {
                // 先把已累积的冲出去
                if (!current.isEmpty()) {
                    result << current;
                    current.clear();
                }
                // 硬切。用 maxChars 为单位切块，不尝试找"词边界" —— 
                // 在无标点文本里，任何"边界"判断都不可靠，切了反而更难读。
                int offset = 0;
                while (offset < sentence.length()) {
                    result << sentence.mid(offset, maxChars);
                    offset += maxChars;
                }
                continue;
            }

            if (current.length() + sentence.length() > maxChars && !current.isEmpty()) {
                result << current;
                current = sentence;
            } else {
                current += sentence;
            }
        }

        if (!current.isEmpty()) {
            result << current;
        }
    }

    return result;
}

// ============================================================================
// TranslationService 构造
// ============================================================================
TranslationService::TranslationService(QObject *parent)
    : QObject(parent)
{
    // 离线词典后端永远先注册 —— 它是兜底，必须存在
    auto *offline = new OfflineDictionaryBackend(this);
    registerBackend(offline);
    m_defaultBackendId = offline->id();
}

TranslationService::~TranslationService() = default;

void TranslationService::setNetworkService(NetworkService *networkService)
{
    m_networkService = networkService;
    // 把已注册的 HTTP 后端也接上
    const QList<TranslationBackend *> all = backends();
    for (TranslationBackend *b : all) {
        if (auto *http = qobject_cast<HttpTranslationBackend *>(b)) {
            http->setNetworkService(networkService);
            http->setSecureStore(m_secureStore);
        }
    }
}

void TranslationService::setSecureStore(SecureStore *secureStore)
{
    m_secureStore = secureStore;
    const QList<TranslationBackend *> all = backends();
    for (TranslationBackend *b : all) {
        if (auto *http = qobject_cast<HttpTranslationBackend *>(b)) {
            http->setSecureStore(secureStore);
        }
    }
}

// ============================================================================
// 后端管理
// ============================================================================
void TranslationService::registerBackend(TranslationBackend *backend)
{
    if (!backend) {
        return;
    }
    const QString id = backend->id();
    if (id.isEmpty()) {
        DC_WARN(LogCat::Translate, QStringLiteral("翻译后端 id 为空，拒绝注册"));
        return;
    }

    if (m_backends.contains(id)) {
        DC_INFO(LogCat::Translate, QStringLiteral("替换翻译后端：%1").arg(id));
        TranslationBackend *old = m_backends.value(id);
        old->disconnect(this);
        if (old->parent() == this) {
            old->deleteLater();
        }
    }

    m_backends.insert(id, backend);
    if (backend->parent() != this && !backend->parent()) {
        backend->setParent(this);
    }

    // 统一在这里转接 finished 信号，后端实现不需要知道 TranslationService 的存在
    const QPointer<TranslationBackend> safeBackend(backend);
    connect(backend, &TranslationBackend::finished, this,
            [this, safeBackend](const TranslationResult &result) {
                if (safeBackend) {
                    onBackendFinished(safeBackend.data(), result);
                }
            });

    DC_INFO(LogCat::Translate, QStringLiteral("已注册翻译后端：%1（%2，需要联网=%3）")
                .arg(id, backend->displayName(),
                     backend->requiresNetwork() ? QStringLiteral("是") : QStringLiteral("否")));
}

void TranslationService::unregisterBackend(const QString &backendId)
{
    auto it = m_backends.find(backendId);
    if (it == m_backends.end()) {
        return;
    }
    TranslationBackend *b = it.value();
    b->disconnect(this);
    m_backends.erase(it);
    if (b->parent() == this) {
        b->deleteLater();
    }
    if (m_defaultBackendId == backendId) {
        m_defaultBackendId = m_backends.isEmpty() ? QString() : m_backends.keys().first();
    }
}

QList<TranslationBackend *> TranslationService::backends() const
{
    return m_backends.values();
}

TranslationBackend *TranslationService::backend(const QString &id) const
{
    return m_backends.value(id, nullptr);
}

void TranslationService::setDefaultBackend(const QString &id)
{
    if (id.isEmpty() || m_backends.contains(id)) {
        m_defaultBackendId = id;
    } else {
        DC_WARN(LogCat::Translate, QStringLiteral("尝试把不存在的翻译后端设为默认：%1").arg(id));
    }
}

QString TranslationService::defaultBackendId() const
{
    return m_defaultBackendId;
}

bool TranslationService::isTranslationAvailable(QString *reason) const
{
    const QList<TranslationBackend *> all = m_backends.values();

    QString firstReason;
    for (TranslationBackend *b : all) {
        QString r;
        if (b->isReady(&r)) {
            return true;
        }
        if (firstReason.isEmpty()) {
            firstReason = r.isEmpty()
                              ? tr("后端 %1 未就绪").arg(b->displayName())
                              : r;
        }
    }

    if (reason) {
        *reason = firstReason.isEmpty()
                      ? tr("没有可用的翻译后端。")
                      : firstReason;
    }
    return false;
}

QString TranslationService::pickUsableBackend(const TranslationRequest &request) const
{
    // 用户显式指定的后端优先，但也要检查它是否就绪
    if (!request.backendId.trimmed().isEmpty()) {
        TranslationBackend *b = m_backends.value(request.backendId, nullptr);
        if (b) {
            QString reason;
            if (b->isReady(&reason)) {
                return request.backendId;
            }
            DC_INFO(LogCat::Translate, QStringLiteral("用户指定的后端 %1 未就绪（%2），尝试自动选择")
                        .arg(request.backendId, reason));
        }
    }

    // 默认后端
    if (!m_defaultBackendId.isEmpty()) {
        TranslationBackend *b = m_backends.value(m_defaultBackendId, nullptr);
        if (b) {
            QString reason;
            if (b->isReady(&reason)) {
                return m_defaultBackendId;
            }
        }
    }

    // 按"能力更强优先"排序挑：需要联网的一般翻译质量更好，
    // 但如果它没配好就跳过。离线词典排在最后。
    QList<TranslationBackend *> candidates = m_backends.values();
    std::sort(candidates.begin(), candidates.end(),
              [](TranslationBackend *a, TranslationBackend *b) {
                  // 需要联网且需要密钥的排最前（通常质量最好，说明用户配过）
                  const int scoreA = (a->requiresNetwork() ? 2 : 0)
                                     + (a->requiresApiKey() ? 1 : 0);
                  const int scoreB = (b->requiresNetwork() ? 2 : 0)
                                     + (b->requiresApiKey() ? 1 : 0);
                  return scoreA > scoreB;
              });

    for (TranslationBackend *b : candidates) {
        QString reason;
        if (b->isReady(&reason)) {
            return b->id();
        }
    }

    // 全都不就绪 -> 返回空，由 translate() 走"失败保留原文"路径
    return QString();
}

// ============================================================================
// 翻译入口
// ============================================================================
QString TranslationService::translate(const TranslationRequest &request)
{
    const QString requestId = QStringLiteral("tr-%1-%2")
                                  .arg(nowMs()).arg(++m_counter);

    const QString text = request.text;
    if (text.trimmed().isEmpty()) {
        // 空文本不是错误，直接成功返回空 —— 免得界面弹一个无意义的错误
        QTimer::singleShot(0, this, [this, requestId, text]() {
            TranslationResult r;
            r.ok = true;
            r.errorCode = ec(erc::Ok);
            r.translatedText = text;
            r.segmentCount = 0;
            emit translationFinished(requestId, r);
        });
        return requestId;
    }

    // 选后端
    const QString backendId = pickUsableBackend(request);
    if (backendId.isEmpty()) {
        QString reason;
        isTranslationAvailable(&reason);
        finishTranslation(requestId, false, ec(erc::SpeechUnavailable),  // 复用"能力不可用"语义
                          reason.isEmpty() ? tr("没有可用的翻译后端。") : reason);
        return requestId;
    }

    TranslationBackend *b = m_backends.value(backendId, nullptr);
    if (!b) {
        finishTranslation(requestId, false, ec(erc::InternalError),
                          tr("翻译后端不可用。"));
        return requestId;
    }

    if (b->requiresNetwork() && !request.allowNetwork) {
        finishTranslation(requestId, false, ec(erc::NetworkOffline),
                          tr("当前设置为不联网，无法使用需要网络连接的翻译后端。"));
        return requestId;
    }

    // 缓存查询
    if (m_cacheEnabled) {
        const QString key = cacheKey(request);
        const auto it = m_cache.constFind(key);
        if (it != m_cache.constEnd()) {
            const QString cached = it.value();
            QTimer::singleShot(0, this, [this, requestId, cached, request, backendId]() {
                TranslationResult r;
                r.ok = true;
                r.errorCode = ec(erc::Ok);
                r.translatedText = cached;
                r.backendId = backendId;
                r.fromCache = true;
                r.detectedSourceLanguage = request.sourceLanguage;
                emit translationFinished(requestId, r);
            });
            return requestId;
        }
    }

    PendingTranslation pending;
    pending.id = requestId;
    pending.request = request;
    pending.backendId = backendId;
    pending.startedAt = nowMs();
    pending.segments = splitText(text, b->maxCharactersPerRequest(),
                                 request.preserveParagraphs);
    if (pending.segments.isEmpty()) {
        pending.segments << text;
    }

    m_pending.insert(requestId, pending);
    m_activeRequestId = requestId;

    DC_INFO(LogCat::Translate, QStringLiteral("开始翻译：后端=%1 段落数=%2 目标语言=%3")
                .arg(backendId)
                .arg(pending.segments.size())
                .arg(normalizeLanguageCode(request.targetLanguage)));

    continueNextSegment(requestId);
    return requestId;
}

void TranslationService::continueNextSegment(const QString &requestId)
{
    auto it = m_pending.find(requestId);
    if (it == m_pending.end()) {
        return;
    }
    if (it->cancelled) {
        finishTranslation(requestId, false, ec(erc::OperationCancelled),
                          tr("翻译已取消。"));
        return;
    }

    // 跳过空段落（原文里的空行），直接保留为译文里的空行
    while (it->currentSegment < it->segments.size()
           && it->segments.at(it->currentSegment).trimmed().isEmpty()) {
        it->translatedSegments << QString();
        ++it->currentSegment;
    }

    if (it->currentSegment >= it->segments.size()) {
        // 全部段落完成 —— 拼回译文
        TranslationResult result;
        result.ok = true;
        result.errorCode = ec(erc::Ok);
        result.backendId = it->backendId;
        result.segmentCount = it->segments.size();
        result.elapsedMs = nowMs() - it->startedAt;
        result.detectedSourceLanguage = it->request.sourceLanguage;

        const QString joined = it->translatedSegments.join(QLatin1Char('\n'));
        result.translatedText = joined;

        const QString reqId = it->id;
        const TranslationRequest originalRequest = it->request;
        m_pending.erase(it);
        if (m_activeRequestId == reqId) {
            m_activeRequestId.clear();
        }

        if (m_cacheEnabled) {
            m_cache.insert(cacheKey(originalRequest), joined);
        }

        DC_INFO(LogCat::Translate, QStringLiteral("翻译完成：%1 段，用时 %2ms")
                    .arg(result.segmentCount).arg(result.elapsedMs));
        emit translationFinished(reqId, result);
        return;
    }

    TranslationBackend *b = m_backends.value(it->backendId, nullptr);
    if (!b) {
        finishTranslation(requestId, false, ec(erc::InternalError),
                          tr("翻译后端在使用过程中变得不可用。"));
        return;
    }

    TranslationRequest segmentRequest = it->request;
    segmentRequest.text = it->segments.at(it->currentSegment);
    segmentRequest.backendId = it->backendId;

    m_activeRequestId = requestId;
    b->translate(segmentRequest);
}

void TranslationService::onBackendFinished(TranslationBackend *backend,
                                            const TranslationResult &result)
{
    Q_UNUSED(backend)

    const QString requestId = m_activeRequestId;
    if (requestId.isEmpty()) {
        // 没有等待中的请求 —— 可能是取消后迟到的回调，忽略即可
        return;
    }

    auto it = m_pending.find(requestId);
    if (it == m_pending.end()) {
        return;
    }

    if (it->cancelled) {
        finishTranslation(requestId, false, ec(erc::OperationCancelled),
                          tr("翻译已取消。"));
        return;
    }

    if (!result.ok) {
        // 后端失败：尝试换一个后端。
        // 这是"可替换后端"设计带来的实际好处 —— 主后端挂了还能兜底。
        QString alternateId;
        const QList<TranslationBackend *> all = m_backends.values();
        for (TranslationBackend *b : all) {
            if (b->id() == it->backendId) {
                continue;
            }
            QString reason;
            if (b->isReady(&reason)) {
                alternateId = b->id();
                break;
            }
        }

        if (!alternateId.isEmpty()) {
            DC_INFO(LogCat::Translate, QStringLiteral("翻译后端 %1 失败（%2），改用 %3 重试")
                        .arg(it->backendId, result.message, alternateId));
            it->backendId = alternateId;
            continueNextSegment(requestId);
            return;
        }

        finishTranslation(requestId, false,
                          result.errorCode.isEmpty() ? ec(erc::InternalError)
                                                     : result.errorCode,
                          result.message);
        return;
    }

    // 成功：记下这一段，继续下一段
    it->translatedSegments << result.translatedText;
    if (it->request.sourceLanguage.isEmpty()
        || normalizeLanguageCode(it->request.sourceLanguage) == QLatin1String("auto")) {
        // 用第一段的检测结果补上源语言，避免每段都重新检测
        if (!result.detectedSourceLanguage.isEmpty()) {
            it->request.sourceLanguage = result.detectedSourceLanguage;
        }
    }
    ++it->currentSegment;

    continueNextSegment(requestId);
}

void TranslationService::finishTranslation(const QString &requestId, bool ok,
                                           const QString &errorCode,
                                           const QString &message)
{
    auto it = m_pending.find(requestId);
    QString originalText;
    TranslationRequest originalRequest;
    QString backendId;
    qint64 startedAt = nowMs();
    int segmentCount = 0;

    if (it != m_pending.end()) {
        originalText = it->request.text;
        originalRequest = it->request;
        backendId = it->backendId;
        startedAt = it->startedAt;
        segmentCount = it->segments.size();
        m_pending.erase(it);
    }
    if (m_activeRequestId == requestId) {
        m_activeRequestId.clear();
    }

    TranslationResult result;
    result.ok = ok;
    result.errorCode = errorCode;
    result.message = message;
    result.backendId = backendId;
    result.segmentCount = segmentCount;
    result.elapsedMs = nowMs() - startedAt;

    // 核心约定：失败时返回原文，并明确标记。
    // 调用方读 isActuallyTranslated() 就知道该不该展示译文。
    result.translatedText = originalText;
    result.usedFallbackToOriginal = !ok;

    if (!ok) {
        DC_WARN(LogCat::Translate, QStringLiteral("翻译失败（%1）：%2").arg(errorCode, message));
        emit translationUnavailable(requestId, message);
    }

    emit translationFinished(requestId, result);
}

// ============================================================================
// 取消
// ============================================================================
bool TranslationService::cancel(const QString &requestId)
{
    auto it = m_pending.find(requestId);
    if (it == m_pending.end()) {
        return false;
    }
    it->cancelled = true;

    if (!it->backendId.isEmpty()) {
        TranslationBackend *b = m_backends.value(it->backendId, nullptr);
        if (b) {
            b->cancel();
        }
    }

    finishTranslation(requestId, false, ec(erc::OperationCancelled), tr("翻译已取消。"));
    return true;
}

void TranslationService::cancelAll()
{
    const QStringList ids = m_pending.keys();
    for (const QString &id : ids) {
        cancel(id);
    }
}

bool TranslationService::isBusy() const
{
    return !m_pending.isEmpty();
}

// ============================================================================
// 缓存
// ============================================================================
void TranslationService::setCacheEnabled(bool enabled)
{
    m_cacheEnabled = enabled;
    if (!enabled) {
        m_cache.clear();
    }
}

bool TranslationService::isCacheEnabled() const
{
    return m_cacheEnabled;
}

void TranslationService::clearCache()
{
    m_cache.clear();
}

int TranslationService::cacheEntryCount() const
{
    return m_cache.size();
}

QString TranslationService::cacheKey(const TranslationRequest &request) const
{
    return QStringLiteral("%1|%2|%3")
        .arg(normalizeLanguageCode(request.sourceLanguage),
             normalizeLanguageCode(request.targetLanguage),
             request.text);
}

// ============================================================================
// OfflineDictionaryBackend
// ============================================================================
OfflineDictionaryBackend::OfflineDictionaryBackend(QObject *parent)
    : TranslationBackend(parent)
{
    loadBuiltinEntries();
}

OfflineDictionaryBackend::~OfflineDictionaryBackend() = default;

QString OfflineDictionaryBackend::id() const
{
    return QStringLiteral("offline_dict");
}

QString OfflineDictionaryBackend::displayName() const
{
    return tr("离线词典（内置）");
}

QStringList OfflineDictionaryBackend::supportedSourceLanguages() const
{
    return { QStringLiteral("zh"), QStringLiteral("en"), QStringLiteral("ja") };
}

QStringList OfflineDictionaryBackend::supportedTargetLanguages() const
{
    return { QStringLiteral("zh"), QStringLiteral("en"), QStringLiteral("ja") };
}

bool OfflineDictionaryBackend::isReady(QString *reason) const
{
    Q_UNUSED(reason)
    // 离线词典永远就绪 —— 这是它存在的意义
    return true;
}

int OfflineDictionaryBackend::entryCount() const
{
    return m_entries.size();
}

QString OfflineDictionaryBackend::defaultDictionaryDirectory()
{
    // 与程序同级的 dictionaries 目录。用户把自己的词表放进去即可扩充。
    return QDir(QCoreApplication::applicationDirPath())
        .filePath(QStringLiteral("dictionaries"));
}

// ---------------------------------------------------------------------------
// 内置词表
//
//  内容刻意保持很小：这些是"界面自身会用到"的词，
//  以及用户最可能试一下的高频词。不做成一个大词库，
//  因为大词库会显著增加体积，而翻译质量依然远不如在线服务 —— 
//  与其让用户以为它能翻译，不如让它明确地说"我只认识这些词"。
// ---------------------------------------------------------------------------
void OfflineDictionaryBackend::loadBuiltinEntries()
{
    struct Entry { const char *from; const char *to; const char *a; const char *b; };

    // zh <-> en
    static const Entry kZhEn[] = {
        { "你好",       "hello",        "zh", "en" },
        { "谢谢",       "thank you",    "zh", "en" },
        { "再见",       "goodbye",      "zh", "en" },
        { "早上好",     "good morning", "zh", "en" },
        { "晚上好",     "good evening", "zh", "en" },
        { "对不起",     "sorry",        "zh", "en" },
        { "请",         "please",       "zh", "en" },
        { "是",         "yes",          "zh", "en" },
        { "不是",       "no",           "zh", "en" },
        { "你好吗",     "how are you",  "zh", "en" },
        { "我不明白",   "I don't understand", "zh", "en" },
        { "请再说一遍", "please say that again", "zh", "en" },
        { "多少钱",     "how much is it", "zh", "en" },
        { "洗手间在哪里", "where is the restroom", "zh", "en" },
        { "帮助",       "help",         "zh", "en" },
        { "文件",       "file",         "zh", "en" },
        { "文件夹",     "folder",       "zh", "en" },
        { "设置",       "settings",     "zh", "en" },
        { "翻译",       "translate",    "zh", "en" },
        { "搜索",       "search",       "zh", "en" },
    };

    for (const Entry &e : kZhEn) {
        m_entries.insert(QStringLiteral("%1|%2|%3")
                             .arg(QString::fromLatin1(e.a),
                                  QString::fromLatin1(e.b),
                                  QString::fromLatin1(e.from).toLower()),
                         QString::fromLatin1(e.to));
        // 反向
        m_entries.insert(QStringLiteral("%1|%2|%3")
                             .arg(QString::fromLatin1(e.b),
                                  QString::fromLatin1(e.a),
                                  QString::fromLatin1(e.to).toLower()),
                         QString::fromLatin1(e.from));
    }

    // zh <-> ja
    static const Entry kZhJa[] = {
        { "你好",   "こんにちは",     "zh", "ja" },
        { "谢谢",   "ありがとう",     "zh", "ja" },
        { "再见",   "さようなら",     "zh", "ja" },
        { "早上好", "おはよう",       "zh", "ja" },
        { "对不起", "すみません",     "zh", "ja" },
        { "是",     "はい",           "zh", "ja" },
        { "不是",   "いいえ",         "zh", "ja" },
    };

    for (const Entry &e : kZhJa) {
        m_entries.insert(QStringLiteral("%1|%2|%3")
                             .arg(QString::fromLatin1(e.a),
                                  QString::fromLatin1(e.b),
                                  QString::fromLatin1(e.from).toLower()),
                         QString::fromLatin1(e.to));
        m_entries.insert(QStringLiteral("%1|%2|%3")
                             .arg(QString::fromLatin1(e.b),
                                  QString::fromLatin1(e.a),
                                  QString::fromLatin1(e.to).toLower()),
                         QString::fromLatin1(e.from));
    }

    DC_INFO(LogCat::Translate, QStringLiteral("离线词典已加载 %1 条词条").arg(m_entries.size()));
}

bool OfflineDictionaryBackend::loadUserDictionary(const QString &filePath,
                                                  QString *errorMessage)
{
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        if (errorMessage) {
            *errorMessage = tr("无法读取词典文件：%1").arg(filePath);
        }
        return false;
    }

    QTextStream stream(&file);
    stream.setEncoding(QStringConverter::Utf8);

    int loaded = 0;
    int lineNumber = 0;
    while (!stream.atEnd()) {
        ++lineNumber;
        const QString line = stream.readLine().trimmed();

        // 支持 # 注释与空行
        if (line.isEmpty() || line.startsWith(QLatin1Char('#'))) {
            continue;
        }

        // 格式：源语言|目标语言|原文=译文
        const int firstPipe = line.indexOf(QLatin1Char('|'));
        const int secondPipe = firstPipe >= 0
                                   ? line.indexOf(QLatin1Char('|'), firstPipe + 1)
                                   : -1;
        const int equals = line.indexOf(QLatin1Char('='), secondPipe + 1);

        if (firstPipe <= 0 || secondPipe <= 0 || equals <= 0) {
            if (errorMessage && loaded == 0) {
                *errorMessage = tr("词典文件第 %1 行格式不正确，"
                                   "应为「源语言|目标语言|原文=译文」。").arg(lineNumber);
            }
            continue;
        }

        const QString src = line.mid(0, firstPipe).trimmed().toLower();
        const QString dst = line.mid(firstPipe + 1, secondPipe - firstPipe - 1)
                                .trimmed().toLower();
        const QString from = line.mid(secondPipe + 1, equals - secondPipe - 1).trimmed();
        const QString to = line.mid(equals + 1).trimmed();

        if (src.isEmpty() || dst.isEmpty() || from.isEmpty()) {
            continue;
        }

        m_entries.insert(QStringLiteral("%1|%2|%3").arg(src, dst, from.toLower()), to);
        ++loaded;
    }

    if (loaded == 0) {
        if (errorMessage && errorMessage->isEmpty()) {
            *errorMessage = tr("词典文件中没有可用的词条。");
        }
        return false;
    }

    DC_INFO(LogCat::Translate, QStringLiteral("已从 %1 加载 %2 条用户词条")
                .arg(PathGuard::redact(filePath)).arg(loaded));
    return true;
}

void OfflineDictionaryBackend::translate(const TranslationRequest &request)
{
    // 异步发结果，保持与在线后端一致的调用契约
    QTimer::singleShot(0, this, [this, request]() {
        const qint64 started = nowMs();

        TranslationResult result;
        result.backendId = id();
        result.segmentCount = 1;

        const QString src = TranslationService::normalizeLanguageCode(request.sourceLanguage);
        QString dst = TranslationService::normalizeLanguageCode(request.targetLanguage);

        const QString input = request.text.trimmed();

        // 源语言未知时，依次尝试所有已知的源语言
        QStringList sourceCandidates;
        if (src.isEmpty() || src == QLatin1String("auto")) {
            sourceCandidates = supportedSourceLanguages();
        } else {
            sourceCandidates << src;
        }

        for (const QString &candidateSrc : sourceCandidates) {
            if (candidateSrc == dst) {
                continue;
            }
            const QString key = QStringLiteral("%1|%2|%3")
                                    .arg(candidateSrc, dst, input.toLower());
            const auto it = m_entries.constFind(key);
            if (it != m_entries.constEnd()) {
                result.ok = true;
                result.errorCode = ec(erc::Ok);
                result.translatedText = it.value();
                result.detectedSourceLanguage = candidateSrc;
                result.elapsedMs = nowMs() - started;
                emit finished(result);
                return;
            }
        }

        // 查不到 —— 老实说查不到，返回原文并标记
        result.ok = false;
        result.errorCode = ec(erc::NotSupported);
        result.message = tr("离线词典中没有收录「%1」。"
                            "离线词典只包含少量常用词，如果需要完整翻译，"
                            "请在设置中配置一个在线翻译后端。")
                             .arg(input.left(20));
        result.translatedText = request.text;   // 失败保留原文
        result.usedFallbackToOriginal = true;
        result.elapsedMs = nowMs() - started;
        emit finished(result);
    });
}

// ============================================================================
// HttpTranslationBackend —— 通用 HTTP 翻译后端
//
//  这是一个"配置驱动"的通用后端：用户填入某个翻译服务的接口地址、
//  请求模板、以及从哪里取出译文，它就能工作。
//
//  为什么不内置某家具体服务：
//    1. 需求要求后端可替换，硬编码即不可替换；
//    2. 不同用户能访问的服务商不同（网络可达性差异很大）；
//    3. 把某家商业服务的接口细节与配额假设写进代码，会随对方改版而失效，
//       而且隐含了对其服务条款的依赖。
// ============================================================================
HttpTranslationBackend::HttpTranslationBackend(QObject *parent)
    : TranslationBackend(parent)
{
}

HttpTranslationBackend::~HttpTranslationBackend() = default;

void HttpTranslationBackend::setNetworkService(NetworkService *networkService)
{
    m_networkService = networkService;
}

void HttpTranslationBackend::setSecureStore(SecureStore *secureStore)
{
    m_secureStore = secureStore;
}

void HttpTranslationBackend::setConfig(const HttpBackendConfig &config)
{
    m_config = config;
    if (m_config.id.trimmed().isEmpty()) {
        m_config.id = QStringLiteral("http_generic");
    }
    if (m_config.displayName.trimmed().isEmpty()) {
        m_config.displayName = tr("在线翻译（自定义）");
    }
    if (m_config.maxCharsPerRequest <= 0) {
        m_config.maxCharsPerRequest = 2000;
    }
    if (m_config.timeoutMs <= 0) {
        m_config.timeoutMs = 15000;
    }
}

HttpBackendConfig HttpTranslationBackend::config() const
{
    return m_config;
}

QString HttpTranslationBackend::id() const
{
    return m_config.id;
}

QString HttpTranslationBackend::displayName() const
{
    return m_config.displayName.isEmpty() ? tr("在线翻译（自定义）") : m_config.displayName;
}

bool HttpTranslationBackend::requiresApiKey() const
{
    return !m_config.apiKeySecretKey.trimmed().isEmpty();
}

QStringList HttpTranslationBackend::supportedTargetLanguages() const
{
    // 不限制目标语言 —— 具体支持哪些由服务商决定，
    // 我们不该在客户端假造一个可能过时的列表把用户挡在外面。
    QStringList codes;
    const QList<LanguageOption> langs = TranslationService::supportedLanguages();
    for (const LanguageOption &l : langs) {
        codes << l.code;
    }
    return codes;
}

int HttpTranslationBackend::maxCharactersPerRequest() const
{
    return m_config.maxCharsPerRequest;
}

bool HttpTranslationBackend::isReady(QString *reason) const
{
    if (!m_config.enabled) {
        if (reason) {
            *reason = tr("在线翻译后端尚未启用。");
        }
        return false;
    }

    if (m_config.endpoint.trimmed().isEmpty()) {
        if (reason) {
            *reason = tr("在线翻译后端未配置接口地址。");
        }
        return false;
    }

    // 强制 https —— 翻译内容属于用户隐私，明文传输不可接受
    const QUrl endpoint(m_config.endpoint.trimmed(), QUrl::StrictMode);
    if (!endpoint.isValid() || endpoint.host().isEmpty()) {
        if (reason) {
            *reason = tr("在线翻译后端的接口地址无效。");
        }
        return false;
    }
    if (endpoint.scheme().toLower() != QLatin1String("https")) {
        if (reason) {
            *reason = tr("在线翻译后端的接口地址必须是 https，"
                         "否则你的翻译内容会以明文在网络上传输。");
        }
        return false;
    }

    if (!m_networkService) {
        if (reason) {
            *reason = tr("网络服务未就绪。");
        }
        return false;
    }

    // 配置要求密钥，但取不到 -> 未就绪，并明确告诉用户去哪儿配
    if (requiresApiKey()) {
        if (!m_secureStore) {
            if (reason) {
                *reason = tr("无法访问安全存储，因此无法读取翻译服务的密钥。");
            }
            return false;
        }
        if (!m_secureStore->isAvailable()) {
            if (reason) {
                *reason = m_secureStore->riskNotice().isEmpty()
                              ? tr("本机无法安全保存密钥，在线翻译不可用。")
                              : m_secureStore->riskNotice();
            }
            return false;
        }
        const SecretValue secret = m_secureStore->secret(m_config.apiKeySecretKey);
        if (!secret.isValid()) {
            if (reason) {
                *reason = tr("尚未配置翻译服务的密钥，请在设置中填写。");
            }
            return false;
        }
    }

    return true;
}

void HttpTranslationBackend::cancel()
{
    if (m_networkService && !m_pendingNetworkRequestId.isEmpty()) {
        m_networkService->cancel(m_pendingNetworkRequestId);
        m_pendingNetworkRequestId.clear();
    }
}

void HttpTranslationBackend::translate(const TranslationRequest &request)
{
    TranslationResult early;
    early.backendId = id();

    // ---- 前置检查 ----
    QString notReady;
    if (!isReady(&notReady)) {
        early.ok = false;
        early.errorCode = ec(erc::NetworkNotConfigured);
        early.message = notReady;
        early.translatedText = request.text;
        early.usedFallbackToOriginal = true;
        QTimer::singleShot(0, this, [this, early]() { emit finished(early); });
        return;
    }

    // ---- 组装请求体 ----
    QString body = m_config.bodyTemplate;

    const QString srcCode = TranslationService::normalizeLanguageCode(request.sourceLanguage);
    const QString dstCode = TranslationService::normalizeLanguageCode(request.targetLanguage);

    // 语言代码是否用英文名，取决于服务商。这里同时替换两种形式：
    // {source} / {target} 得到规范化代码，{source_name} / {target_name} 得到英文名。
    // 这样配置模板时更灵活，不用为每家服务商改代码。
    body.replace(QStringLiteral("{source_name}"),
                 TranslationService::languageDisplayName(
                     srcCode.isEmpty() ? QStringLiteral("auto") : srcCode));
    body.replace(QStringLiteral("{target_name}"),
                 TranslationService::languageDisplayName(dstCode));
    body.replace(QStringLiteral("{source}"), srcCode.isEmpty() ? QStringLiteral("auto") : srcCode);
    body.replace(QStringLiteral("{target}"), dstCode);
    body.replace(QStringLiteral("{text}"), request.text);

    // GET 时把模板拼成查询串
    QUrl url(m_config.endpoint.trimmed(), QUrl::StrictMode);
    const QByteArray method = m_config.method.trimmed().toUpper().toLatin1();
    if (method == "GET") {
        // 模板里可能是 "text={text}&source={source}" 这种形式，
        // 直接作为 query 附到 endpoint 后面。
        if (!body.trimmed().isEmpty()) {
            url.setQuery(body.trimmed(), QUrl::TolerantMode);
        }
    }

    NetworkRequest netRequest;
    netRequest.requester = tr("翻译");
    netRequest.url = url;
    netRequest.method = method.isEmpty() ? QByteArray("GET") : method;
    netRequest.purpose = tr("把文本翻译成%1")
                             .arg(TranslationService::languageDisplayName(dstCode));
    netRequest.expectText = false;   // 响应是 JSON，需要结构化解析
    netRequest.timeoutMs = m_config.timeoutMs;

    if (netRequest.method == "POST") {
        netRequest.body = body.toUtf8();
        netRequest.headers.append(
            qMakePair(QByteArray("Content-Type"),
                      QByteArray("application/json")));
    }

    // 密钥放进请求头。值来自 SecureStore，时刻短暂存在于内存。
    if (requiresApiKey() && m_secureStore) {
        const SecretValue secret = m_secureStore->secret(m_config.apiKeySecretKey);
        if (secret.isValid()) {
            const QString headerName = m_config.apiKeyHeader.trimmed().isEmpty()
                                           ? QStringLiteral("Authorization")
                                           : m_config.apiKeyHeader.trimmed();
            const QString headerValue = m_config.apiKeyPrefix + secret.value;
            netRequest.headers.append(
                qMakePair(headerName.toUtf8(), headerValue.toUtf8()));
            // 注意：绝不能把 secret.value 写进任何日志。
            // Logger::sanitize() 也会兜底拦截，但这里根本不打印才是正确做法。
        }
    }

    // ---- 提交 ----
    const QString netRequestId = m_networkService->submitApproved(netRequest);
    m_pendingNetworkRequestId = netRequestId;

    // 用临时连接接收结果，避免长期挂着的信号累积
    auto *connection = new QMetaObject::Connection;
    *connection = connect(
        m_networkService, &NetworkService::finished, this,
        [this, connection, netRequestId, request, dstCode](
            const QString &finishedId, const NetworkResponse &response) {

            if (finishedId != netRequestId) {
                return;
            }
            disconnect(*connection);
            delete connection;
            m_pendingNetworkRequestId.clear();

            TranslationResult result;
            result.backendId = id();

            if (!response.ok) {
                result.ok = false;
                result.errorCode = response.errorCode;
                result.message = response.message;
                result.translatedText = request.text;
                result.usedFallbackToOriginal = true;
                emit finished(result);
                return;
            }

            // ---- 解析响应 ----
            QJsonParseError parseError{};
            const QJsonDocument doc =
                QJsonDocument::fromJson(response.body, &parseError);

            if (parseError.error != QJsonParseError::NoError) {
                result.ok = false;
                result.errorCode = ec(erc::NetworkRequestFailed);
                result.message = tr("翻译服务返回的内容无法解析，"
                                    "可能是接口配置不正确或服务已改版。");
                result.translatedText = request.text;
                result.usedFallbackToOriginal = true;
                DC_WARN(LogCat::Translate, QStringLiteral("翻译响应 JSON 解析失败：%1（偏移 %2）")
                            .arg(parseError.errorString()).arg(parseError.offset));
                emit finished(result);
                return;
            }

            // 按 responsePath 取出译文。路径形如 "data.translations.0.text"，
            // 数字段表示数组下标。
            QString extracted;
            bool found = false;

            if (m_config.responsePath.trimmed().isEmpty()) {
                // 没配路径：只有"顶层就是一个 JSON 字符串"这种简单响应
                // 才能直接取用。注意 QJsonDocument 本身没有 isString()/
                // toString()，必须先取出根值再判断类型。
                const QJsonValue root = doc.isArray()
                                            ? QJsonValue(doc.array())
                                            : QJsonValue(doc.object());
                if (root.isString()) {
                    extracted = root.toString();
                    found = true;
                }
            } else {
                const QStringList parts =
                    m_config.responsePath.split(QLatin1Char('.'), Qt::SkipEmptyParts);
                QJsonValue current = doc.isArray()
                                         ? QJsonValue(doc.array())
                                         : QJsonValue(doc.object());
                for (const QString &part : parts) {
                    bool isIndex = false;
                    const int index = part.toInt(&isIndex);
                    if (isIndex) {
                        if (!current.isArray()) {
                            break;
                        }
                        current = current.toArray().at(index);
                    } else {
                        if (!current.isObject()) {
                            break;
                        }
                        current = current.toObject().value(part);
                    }
                    if (current.isUndefined() || current.isNull()) {
                        break;
                    }
                }
                if (current.isString()) {
                    extracted = current.toString();
                    found = true;
                } else if (current.isArray()) {
                    // 有些服务返回 ["译文1","译文2"]，按段落拼起来
                    QStringList chunks;
                    const QJsonArray arr = current.toArray();
                    for (const QJsonValue &v : arr) {
                        if (v.isString()) {
                            chunks << v.toString();
                        }
                    }
                    if (!chunks.isEmpty()) {
                        extracted = chunks.join(QLatin1Char('\n'));
                        found = true;
                    }
                }
            }

            if (!found || extracted.isEmpty()) {
                result.ok = false;
                result.errorCode = ec(erc::NetworkRequestFailed);
                result.message = tr("翻译服务返回的内容里没有找到译文。"
                                    "请检查设置中的响应字段路径是否正确。");
                result.translatedText = request.text;
                result.usedFallbackToOriginal = true;
                DC_WARN(LogCat::Translate, QStringLiteral("翻译响应中未找到译文，responsePath=%1")
                            .arg(m_config.responsePath));
                emit finished(result);
                return;
            }

            result.ok = true;
            result.errorCode = ec(erc::Ok);
            result.translatedText = extracted;
            result.detectedSourceLanguage = request.sourceLanguage;
            result.segmentCount = 1;
            emit finished(result);
        });

    DC_INFO(LogCat::Translate, QStringLiteral("已向在线翻译后端发起请求（%1）")
                .arg(NetworkService::normalizeHost(url.host())));
}

} // namespace dc
