#pragma once

// ============================================================================
//  NetworkService —— 所有网络访问的唯一出口（SRS 3.2 / 4.7）
//
//  需求原文（SRS 4.7 网络访问）：
//    - 提供四种网络模式：离线 / 按次询问 / 白名单 / 全部允许（默认离线）
//    - 按次询问模式下，每次访问前必须征得用户同意
//    - 使用域名与服务白名单
//    - 强制 HTTPS/TLS
//    - 设置超时与最大响应体
//    - 禁止无限重试
//    - （SRS 3.2）UI 层不得直接访问网络，所有网络访问统一通过 NetworkService
//
//  设计要点：
//    1. 默认拒绝。NetworkMode 的默认值是 Offline，不是"询问"。
//       这一点很重要：一个默认联网的程序在离线环境下会卡住、会报错、
//       会被防火墙弹窗——而一个默认离线的程序在任何环境下都"能用"。
//    2. 模式判定与主机白名单是两道独立的门，必须同时通过。
//       白名单里的域名在 Offline 模式下依然不能访问。
//    3. TLS 强制。除非用户显式开启 allow_insecure_tls（配置层会对这个
//       开关做"强制关闭 + 记 Error 级问题"的处理，见 ConfigManager），
//       否则 http:// 一律拒绝。
//    4. 响应体大小上限是"边收边判"的，不是等收完再判。
//       否则一个 10GB 的响应会把内存吃光。
//    5. 重试策略有硬上限，且仅对"可重试错误"（超时、5xx）生效。
//       4xx 重试没有意义，只会浪费用户流量。
//    6. 每个请求都带 requestId，方便取消、审计与界面关联。
// ============================================================================

#include <QObject>
#include <QString>
#include <QStringList>
#include <QHash>
#include <QDateTime>
#include <QUrl>
#include <QByteArray>
#include <QMutex>
#include <QList>
#include <QPair>

#include "model/Types.h"

QT_BEGIN_NAMESPACE
class QNetworkAccessManager;
class QNetworkReply;
class QTimer;
QT_END_NAMESPACE

namespace dc {

// ---------------------------------------------------------------------------
// 请求描述
// ---------------------------------------------------------------------------
struct NetworkRequest {
    // 逻辑发起方，例如 "translation"、"search"。用于审计与界面上说明
    // "是谁想联网"（用户在做决定时需要知道这个）。
    QString requester;

    QUrl url;
    QByteArray method = "GET";                 // GET / POST / HEAD
    QByteArray body;
    QList<QPair<QByteArray, QByteArray>> headers;

    // 本次请求的目的说明，会显示在"是否允许访问网络"的确认框里。
    // 空的话界面只能显示域名，用户较难判断该不该点同意。
    QString purpose;

    // 覆盖默认超时（毫秒）。0 = 用服务默认值。
    int timeoutMs = 0;

    // 覆盖默认响应体上限（字节）。0 = 用服务默认值。
    qint64 maxResponseBytes = 0;

    // 说明这次请求的响应要当作文本处理（影响编码猜测与字符集转换）
    bool expectText = true;
};

// ---------------------------------------------------------------------------
// 响应
// ---------------------------------------------------------------------------
struct NetworkResponse {
    bool ok = false;
    QString errorCode;          // erc:: 常量
    QString message;            // 面向用户
    int httpStatus = 0;
    QByteArray body;            // 已按上限截断的响应体
    bool truncated = false;     // 是否因超限被截断
    QString effectiveUrl;       // 跟随重定向后的最终地址
    int attempts = 0;           // 实际尝试次数（让用户看到"重试过 3 次"）
    qint64 elapsedMs = 0;

    QString text() const;
};

// ---------------------------------------------------------------------------
// 访问授权判定结果（供 UI 弹出确认框用）
// ---------------------------------------------------------------------------
struct NetworkAccessDecision {
    enum class Verdict {
        Allow,          // 直接放行（白名单模式且域名在白名单内 / 完全允许模式）
        AskUser,        // 需要询问用户（按次询问模式）
        Deny            // 拒绝（离线模式 / 不在白名单 / 非 https）
    };
    Verdict verdict = Verdict::Deny;
    QString errorCode;
    QString reason;            // 拒绝原因（面向用户）
    QString askPrompt;         // 需要询问时展示的完整问句
};

// ---------------------------------------------------------------------------
// 服务配置
// ---------------------------------------------------------------------------
struct NetworkConfigData {
    NetworkMode mode = NetworkMode::Offline;

    // 域名白名单（不含协议，支持 *.example.com 形式的子域通配）
    QStringList domainWhitelist;

    // 是否允许明文 http（默认 false。配置层会强制此值为 false）
    bool allowInsecureTls = false;

    // 默认超时（毫秒）
    int timeoutMs = 10000;

    // 默认响应体上限（字节），默认 2MB
    qint64 maxResponseBytes = 2 * 1024 * 1024;

    // 最大尝试次数（含首次）。上限 3。
    int maxAttempts = 2;

    // 是否允许跟随重定向。开启时会校验每一跳的目标域名是否仍在白名单内。
    bool followRedirects = true;

    // 最多跟随几跳
    int maxRedirects = 3;

    // 连接超时（毫秒），与整体超时分开，便于区分"连不上"和"连上了但不回应"
    int connectTimeoutMs = 5000;
};

// ============================================================================
// NetworkService
// ============================================================================
class NetworkService : public QObject
{
    Q_OBJECT

public:
    explicit NetworkService(QObject *parent = nullptr);
    ~NetworkService() override;

    // 应用配置。可在运行期调用（用户在设置里改了模式立刻生效）。
    void applyConfig(const NetworkConfigData &config);
    NetworkConfigData config() const;
    NetworkMode mode() const;
    void setMode(NetworkMode mode);

    // 域名白名单管理（设置页使用）
    QStringList domainWhitelist() const;
    void setDomainWhitelist(const QStringList &domains);
    bool isHostWhitelisted(const QString &host) const;

    // ---- 前置判定（不发请求）----
    // 界面可以在用户点"翻译"之前先问一下，避免点了半天才弹确认框。
    NetworkAccessDecision decide(const NetworkRequest &request) const;

    // ---- 执行请求 ----
    // 异步。返回 requestId（空表示在提交阶段就被拒绝）。
    // 结果通过 finished 信号返回。
    //
    // 注意：本函数自己不弹确认框 —— 确认必须由 UI 完成，否则服务层
    // 就要依赖 UI，违反分层。正确用法：
    //   1. decide() 得到 AskUser
    //   2. UI 弹框，用户选择
    //   3. 用户同意则调用 submitApproved()，否则什么都不做并记录拒绝
    QString submit(const NetworkRequest &request);

    // 用户已同意后提交（decide 返回 AskUser 且用户点了同意时调用）
    QString submitApproved(const NetworkRequest &request);

    // 取消某个请求
    bool cancel(const QString &requestId);
    void cancelAll();

    // 是否正在有请求在跑（界面上转圈用）
    bool isBusy() const;
    int activeRequestCount() const;

    // ---- 用户决策记录（由 UI 回调）----
    // 用户在"按次询问"里选择了"本次允许/本次拒绝/始终允许此域名/始终拒绝"
    void reportUserDecision(const NetworkRequest &request,
                            bool allowed,
                            bool rememberForHost);

    // 某个域名是否被用户永久拒绝（拒绝优先级高于白名单和全部允许模式）
    bool isHostUserDenied(const QString &host) const;
    void clearHostDenials();

    // ---- 会话统计（可选展示）----
    struct SessionStats {
        int totalRequests = 0;
        int allowedByMode = 0;
        int userApproved = 0;
        int refusedByUser = 0;
        int blockedByPolicy = 0;
        int failed = 0;
        qint64 bytesReceived = 0;
    };
    SessionStats sessionStats() const;
    void resetSessionStats();

    // 主机名规范化（小写、去掉端口、处理 IDN）——也用于白名单比对，公开以便测试
    static QString normalizeHost(const QString &host);

    // 通配符白名单匹配，公开以便单元测试
    static bool hostMatchesPattern(const QString &host, const QString &pattern);

signals:
    // 请求结束（成功或失败都会发）
    void finished(const QString &requestId, const dc::NetworkResponse &response);

    // 需要界面弹确认框（当 submit 在 AskUser 模式下被直接调用时发出，
    // 作为兜底；正常流程应当先 decide）
    void confirmationRequired(const QString &requestId, const dc::NetworkRequest &request);

    // 安全事件：TLS 降级尝试、重定向到未授权域名等
    void securityNotice(const QString &message);

    // 会话统计变化
    void statsChanged();

private:
    struct PendingRequest {
        NetworkRequest request;
        QString id;
        int attemptsLeft = 0;
        qint64 startedAt = 0;
        QNetworkReply *reply = nullptr;
        QTimer *timer = nullptr;
        bool cancelled = false;
    };

    QString startRequest(const NetworkRequest &request);

    // 用指定的 requestId 发送。重试与重定向复用同一个 id，
    // 这样调用方只需关心一次"逻辑请求"的生命周期，而不用感知内部做了几跳。
    void startRequestWithId(const NetworkRequest &request, const QString &requestId);

    void handleReplyFinished(const QString &requestId);
    void handleTimeout(const QString &requestId);
    void finishWithError(const QString &requestId, const QString &errorCode,
                         const QString &message);
    QString nextRequestId();

    // 校验 URL 是否符合当前模式与安全要求
    NetworkAccessDecision evaluate(const NetworkRequest &request) const;

    // 重定向目标是否允许（防止被重定向到未授权域名或降级到 http）
    bool redirectTargetAllowed(const QUrl &from, const QUrl &to, QString *reason) const;

    bool isRetriableStatus(int status) const;

    mutable QMutex m_mutex;
    NetworkConfigData m_config;
    QNetworkAccessManager *m_manager = nullptr;
    QHash<QString, PendingRequest> m_pending;
    QStringList m_userDeniedHosts;
    SessionStats m_stats;
    quint64 m_idCounter = 0;
};

} // namespace dc

Q_DECLARE_METATYPE(dc::NetworkResponse)
Q_DECLARE_METATYPE(dc::NetworkRequest)
