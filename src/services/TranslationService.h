#pragma once

// ============================================================================
//  TranslationService —— 文本翻译（SRS 4.9）
//
//  需求原文（SRS 4.9 搜索与翻译）：
//    "翻译：提供文本翻译能力，翻译后端可替换。"
//
//  以及 SRS 11（安全与隐私）：
//    "敏感配置和 API 密钥使用操作系统提供的安全存储能力；
//     旧系统不支持时，提供风险说明和替代方案。"
//
//  设计目标：
//    1. 后端可替换（需求硬约束）。所有后端都实现同一个 TranslationBackend
//       接口，TranslationService 只依赖接口，不依赖任何具体服务商。
//    2. 失败保留原文。翻译失败时返回原文 + 明确错误，绝不给用户
//       一个空字符串或者一段胡说八道的机器结果。
//    3. 内置一个"离线词典"后端作为最后兜底：查不到就老实说查不到。
//       这比"联网失败后静默返回原文、用户以为翻译成功"要好得多。
//    4. 密钥从 SecureStore 取，不从配置文件取。
//       配置文件里只存"用哪个后端"和"密钥的键名"。
//    5. 长文本按段落切分后逐段翻译，避免单次请求过大被拒；
//       同时保持段落结构，不然译文会变成一大坨。
// ============================================================================

#include <QObject>
#include <QString>
#include <QStringList>
#include <QList>
#include <QHash>
#include <QSharedPointer>

#include "model/Types.h"

namespace dc {

class NetworkService;
class SecureStore;

// ---------------------------------------------------------------------------
// 翻译请求
// ---------------------------------------------------------------------------
struct TranslationRequest {
    QString text;
    QString sourceLanguage;      // "" 或 "auto" = 自动检测
    QString targetLanguage;      // "zh" / "en" / "ja" ...
    QString backendId;           // 空 = 用配置的默认后端
    bool preserveParagraphs = true;
    bool allowNetwork = true;    // 后端需要联网时是否允许（受网络模式约束）
};

// ---------------------------------------------------------------------------
// 翻译结果
// ---------------------------------------------------------------------------
struct TranslationResult {
    bool ok = false;
    QString errorCode;
    QString message;             // 面向用户
    QString translatedText;      // 失败时为原文（见类注释第 2 条）
    QString detectedSourceLanguage;
    QString backendId;
    bool usedFallbackToOriginal = false;   // true = 翻译没成功，返回的是原文
    bool fromCache = false;
    int segmentCount = 0;
    qint64 elapsedMs = 0;

    // 便捷判定：让调用方能一眼看出"这到底是不是翻译成功"
    bool isActuallyTranslated() const { return ok && !usedFallbackToOriginal; }
};

// ---------------------------------------------------------------------------
// 语言条目
// ---------------------------------------------------------------------------
struct LanguageOption {
    QString code;                // ISO 639-1，可能带连字符，如 "zh-Hans"
    QString displayName;         // 显示名（本地化的）
    QString englishName;         // 用于拼接 API 参数
};

// ============================================================================
// TranslationBackend —— 可替换后端接口
//
//  实现这个接口就能接入新的翻译服务，不需要改 TranslationService。
//  两个必须遵守的约定：
//    1. translate() 必须在完成时发 finished 信号（成功或失败都要），
//       否则调用方会一直等下去。
//    2. translate() 不得抛异常。所有错误通过结果对象返回。
// ============================================================================
class TranslationBackend : public QObject
{
    Q_OBJECT

public:
    explicit TranslationBackend(QObject *parent = nullptr) : QObject(parent) {}
    ~TranslationBackend() override = default;

    // 后端标识，例如 "offline_dict" / "http_generic"
    virtual QString id() const = 0;
    virtual QString displayName() const = 0;

    // 是否需要联网
    virtual bool requiresNetwork() const = 0;

    // 是否需要 API 密钥
    virtual bool requiresApiKey() const = 0;

    // 支持的源语言（空列表 = 支持自动检测）
    virtual QStringList supportedSourceLanguages() const { return QStringList(); }

    // 支持的目标语言
    virtual QStringList supportedTargetLanguages() const = 0;

    // 是否已就绪（配置齐全、密钥已取到等）。
    // 未就绪时 reason 说明原因，界面据此把该后端置灰并显示原因（SRS 6.2 精神）。
    virtual bool isReady(QString *reason = nullptr) const = 0;

    // 单次请求能处理的最大字符数
    virtual int maxCharactersPerRequest() const { return 2000; }

    // 执行翻译。必须异步完成并发 finished 信号。
    virtual void translate(const TranslationRequest &request) = 0;

    // 取消当前进行中的翻译（尽力而为，不保证一定成功）
    virtual void cancel() {}

signals:
    void finished(const dc::TranslationResult &result);
};

// ============================================================================
// OfflineDictionaryBackend —— 离线词典兜底后端
//
//  为什么要有它：
//    在离线模式、或用户没配置任何在线后端时，翻译功能如果直接报
//    "网络已禁用"，用户会觉得功能坏了。这里提供一个诚实的兜底：
//    用内置的常用词表翻译，查不到就明确说"离线词典中没有这个词"。
//    用户得到的信息是准确的，而不是被误导。
// ============================================================================
class OfflineDictionaryBackend : public TranslationBackend
{
    Q_OBJECT

public:
    explicit OfflineDictionaryBackend(QObject *parent = nullptr);
    ~OfflineDictionaryBackend() override;

    QString id() const override;
    QString displayName() const override;
    bool requiresNetwork() const override { return false; }
    bool requiresApiKey() const override { return false; }
    QStringList supportedSourceLanguages() const override;
    QStringList supportedTargetLanguages() const override;
    bool isReady(QString *reason) const override;
    int maxCharactersPerRequest() const override { return 128; }

    void translate(const TranslationRequest &request) override;

    // 词典条目数（设置页可展示，让用户知道这个兜底有多弱）
    int entryCount() const;

    // 加载额外的用户词典（"原文=译文" 逐行，UTF-8）
    bool loadUserDictionary(const QString &filePath, QString *errorMessage = nullptr);

    // 词典目录结构（用于向用户说明"你可以扩充它"）
    static QString defaultDictionaryDirectory();

private:
    // key = "源语言|目标语言|原文小写"
    QHash<QString, QString> m_entries;
    void loadBuiltinEntries();
};

// ============================================================================
// HttpTranslationBackend —— 通用 HTTP 翻译后端（可配置服务商）
//
//  设计成"配置驱动"而不是硬编码某家服务商，原因：
//    - 需求要求后端可替换，硬编码就变成不可替换了；
//    - 不同用户能访问的服务商不同；
//    - 避免把某个商业服务的条款与配额假设写死在代码里。
//
//  配置项：
//    endpoint      : 请求地址（必须 https）
//    method        : GET / POST
//    apiKeyHeader  : 密钥放在哪个请求头（POST 时也可以放 body 模板）
//    apiKeySecretKey: SecureStore 里的键名（不是密钥本身！）
//    bodyTemplate  : POST 时的请求体模板，支持 {text} {source} {target}
//    responsePath  : 从 JSON 响应中取出译文的路径，如 "data.translations.0.text"
// ============================================================================
struct HttpBackendConfig {
    QString id = QStringLiteral("http_generic");
    QString displayName;
    QString endpoint;
    QString method = QStringLiteral("POST");
    QString apiKeyHeader;          // 例如 "Authorization"
    QString apiKeyPrefix;          // 例如 "Bearer "
    QString apiKeySecretKey;       // SecureStore 键名
    QString bodyTemplate;
    QString responsePath;
    QString textParamName = QStringLiteral("text");
    QString sourceParamName = QStringLiteral("source");
    QString targetParamName = QStringLiteral("target");
    int timeoutMs = 15000;
    int maxCharsPerRequest = 2000;
    bool enabled = false;
};

class HttpTranslationBackend : public TranslationBackend
{
    Q_OBJECT

public:
    explicit HttpTranslationBackend(QObject *parent = nullptr);
    ~HttpTranslationBackend() override;

    void setNetworkService(NetworkService *networkService);
    void setSecureStore(SecureStore *secureStore);
    void setConfig(const HttpBackendConfig &config);
    HttpBackendConfig config() const;

    QString id() const override;
    QString displayName() const override;
    bool requiresNetwork() const override { return true; }
    bool requiresApiKey() const override;
    QStringList supportedTargetLanguages() const override;
    bool isReady(QString *reason) const override;
    int maxCharactersPerRequest() const override;

    void translate(const TranslationRequest &request) override;
    void cancel() override;

signals:
    // 需要界面询问用户"是否允许访问网络"时发出
    void networkConfirmationRequired(const QString &host, const QString &purpose);

private:
    QString m_pendingNetworkRequestId;
    HttpBackendConfig m_config;
    NetworkService *m_networkService = nullptr;
    SecureStore *m_secureStore = nullptr;
};

// ============================================================================
// TranslationService
// ============================================================================
class TranslationService : public QObject
{
    Q_OBJECT

public:
    explicit TranslationService(QObject *parent = nullptr);
    ~TranslationService() override;

    // 依赖注入
    void setNetworkService(NetworkService *networkService);
    void setSecureStore(SecureStore *secureStore);

    // ---- 后端管理 ----

    // 注册后端。同 id 会覆盖。注册时会自动接上 finished 信号转发。
    void registerBackend(TranslationBackend *backend);
    void unregisterBackend(const QString &backendId);
    QList<TranslationBackend *> backends() const;
    TranslationBackend *backend(const QString &id) const;

    void setDefaultBackend(const QString &id);
    QString defaultBackendId() const;

    // 综合判断：现在到底能不能翻译？
    // 返回 false 时 reason 说明原因（界面用于置灰按钮）
    bool isTranslationAvailable(QString *reason = nullptr) const;

    // 自动挑一个可用后端：优先默认，其次第一个就绪的，
    // 最后回退到离线词典（离线词典总是就绪）
    QString pickUsableBackend(const TranslationRequest &request) const;

    // ---- 翻译 ----

    // 异步翻译。返回 requestId。
    // 完成时发 translationFinished 信号。
    //
    // 行为约定（失败保留原文）：
    //   - 后端返回失败 -> translatedText = 原文，usedFallbackToOriginal = true
    //   - 所有后端都不可用 -> 同上，并给出"为什么不可用"的原因
    //   调用方只需读 result.isActuallyTranslated() 就知道该不该展示译文。
    QString translate(const TranslationRequest &request);

    // 取消
    bool cancel(const QString &requestId);
    void cancelAll();
    bool isBusy() const;

    // ---- 语言 ----

    // 内置语言表（不依赖后端，因为界面需要在没有后端时也能显示语言选择）
    static QList<LanguageOption> supportedLanguages();

    // 语言代码 -> 显示名
    static QString languageDisplayName(const QString &code);

    // 规范化语言代码（"zh-CN" -> "zh"，"EN" -> "en"）
    static QString normalizeLanguageCode(const QString &code);

    // 为自动检测出的语言挑目标语言（例如源是中文就给英文，反之亦然）
    static QString suggestTargetLanguage(const QString &detectedSource);

    // ---- 文本切分 ----

    // 按段落/句子切分文本，保证每段不超过 maxChars。
    // 公开以便单元测试 —— 切分出问题是翻译类功能最常见的 bug 来源。
    static QStringList splitText(const QString &text, int maxChars, bool preserveParagraphs);

    // ---- 缓存 ----

    void setCacheEnabled(bool enabled);
    bool isCacheEnabled() const;
    void clearCache();
    int cacheEntryCount() const;

signals:
    void translationFinished(const QString &requestId, const dc::TranslationResult &result);

    // 翻译被拒绝（因为网络策略、密钥缺失等），用于界面提示
    void translationUnavailable(const QString &requestId, const QString &reason);

private:
    struct PendingTranslation {
        QString id;
        TranslationRequest request;
        QString backendId;
        QStringList segments;
        int currentSegment = 0;
        QStringList translatedSegments;
        qint64 startedAt = 0;
        bool cancelled = false;
    };

    void onBackendFinished(TranslationBackend *backend, const TranslationResult &result);
    void continueNextSegment(const QString &requestId);
    void finishTranslation(const QString &requestId, bool ok, const QString &errorCode,
                           const QString &message);
    QString cacheKey(const TranslationRequest &request) const;

    NetworkService *m_networkService = nullptr;
    SecureStore *m_secureStore = nullptr;

    QHash<QString, TranslationBackend *> m_backends;
    QString m_defaultBackendId;

    QHash<QString, PendingTranslation> m_pending;
    // 记录"这次后端调用属于哪个 requestId"，因为后端只发一个全局 finished
    QString m_activeRequestId;

    QHash<QString, QString> m_cache;
    bool m_cacheEnabled = true;
    quint64 m_counter = 0;
};

} // namespace dc

Q_DECLARE_METATYPE(dc::TranslationResult)
Q_DECLARE_METATYPE(dc::TranslationRequest)
