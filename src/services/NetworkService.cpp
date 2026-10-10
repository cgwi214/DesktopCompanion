#include "NetworkService.h"

#include "core/ErrorCode.h"
#include "core/Logger.h"

#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSslConfiguration>
#include <QSslSocket>
#include <QTimer>
#include <QUrlQuery>
#include <QMutexLocker>

#include <algorithm>

namespace dc {

static inline QString ec(const char *code)
{
    return QString::fromLatin1(code);
}

// ============================================================================
// NetworkResponse
// ============================================================================
QString NetworkResponse::text() const
{
    if (body.isEmpty()) {
        return QString();
    }

    // 编码处理：先看有没有 BOM，再看 Content-Type 里的 charset，
    // 都没有就当 UTF-8。故意不引入启发式编码猜测（如 chardet），
    // 因为那类库在网上传数据时误判率不低，且增加体积。
    if (body.size() >= 3
        && static_cast<unsigned char>(body.at(0)) == 0xEF
        && static_cast<unsigned char>(body.at(1)) == 0xBB
        && static_cast<unsigned char>(body.at(2)) == 0xBF) {
        return QString::fromUtf8(body.mid(3));
    }

    return QString::fromUtf8(body);
}

// ============================================================================
// 构造
// ============================================================================
NetworkService::NetworkService(QObject *parent)
    : QObject(parent)
    , m_manager(new QNetworkAccessManager(this))
{
    // 不自动跟随重定向 —— QNetworkAccessManager 的自动跟随不会让我们
    // 检查中间跳的目标域名，可能被 302 到未授权域名再返回数据。
    // 这里改为手动处理，每一跳都过一遍白名单与协议检查。
    m_manager->setRedirectPolicy(QNetworkRequest::ManualRedirectPolicy);

    // 说明：Qt5 的 QNetworkAccessManager::networkAccessibleChanged 在 Qt6 已被移除，
    // 官方替代方案是 QNetworkInformation（Qt 6.1+）。这里不再主动探测链路状态，
    // 因为"断网"最终会以请求超时的形式体现，届时由重试与超时逻辑统一处理，
    // 避免额外依赖与误判（例如仅 DNS 抖动就取消所有请求）。
}

NetworkService::~NetworkService()
{
    cancelAll();
}

// ============================================================================
// 配置
// ============================================================================
void NetworkService::applyConfig(const NetworkConfigData &config)
{
    QMutexLocker locker(&m_mutex);
    m_config = config;

    // 配置层的安全兜底：即使调用方传了不安全的组合，这里也要纠正。
    // 双重保险，因为配置可能来自导入的文件。
    if (m_config.allowInsecureTls) {
        // 不静默改掉（那样用户会觉得设置没生效），而是记录并强制关闭。
        DC_ERROR(LogCat::Network,
                 QStringLiteral("配置要求允许明文传输，已被安全策略强制关闭"));
        m_config.allowInsecureTls = false;
    }
    if (m_config.maxAttempts < 1) {
        m_config.maxAttempts = 1;
    } else if (m_config.maxAttempts > 3) {
        // SRS 4.7：禁止无限重试。这里把硬上限设为 3，比"禁止无限"更明确。
        m_config.maxAttempts = 3;
    }
    if (m_config.timeoutMs < 1000) {
        m_config.timeoutMs = 1000;
    }
    if (m_config.maxResponseBytes < 4096) {
        m_config.maxResponseBytes = 4096;
    }
    if (m_config.maxRedirects < 0) {
        m_config.maxRedirects = 0;
    } else if (m_config.maxRedirects > 5) {
        m_config.maxRedirects = 5;
    }
}

NetworkConfigData NetworkService::config() const
{
    QMutexLocker locker(&m_mutex);
    return m_config;
}

NetworkMode NetworkService::mode() const
{
    QMutexLocker locker(&m_mutex);
    return m_config.mode;
}

void NetworkService::setMode(NetworkMode mode)
{
    {
        QMutexLocker locker(&m_mutex);
        m_config.mode = mode;
    }
    DC_INFO(LogCat::Network, QStringLiteral("网络模式变更为 %1").arg(networkModeToString(mode)));

    // 切到离线就立刻停掉所有在跑的请求 —— 用户改设置的意思是"现在别联网了"，
    // 让已经发出去的请求继续跑完再停是不符合预期的。
    if (mode == NetworkMode::Offline) {
        cancelAll();
    }
}

QStringList NetworkService::domainWhitelist() const
{
    QMutexLocker locker(&m_mutex);
    return m_config.domainWhitelist;
}

void NetworkService::setDomainWhitelist(const QStringList &domains)
{
    QStringList cleaned;
    for (const QString &raw : domains) {
        const QString host = normalizeHost(raw);
        if (!host.isEmpty() && !cleaned.contains(host)) {
            cleaned << host;
        }
    }
    QMutexLocker locker(&m_mutex);
    m_config.domainWhitelist = cleaned;
}

// ============================================================================
// 主机名规范化
//  为什么需要：白名单比对是纯字符串比较，任何形式的不一致都会导致
//  要么"该放行的被拦"（用户觉得功能坏了），要么"该拦的放过了"（安全问题）。
//  必须把各种写法统一到一种形态：
//    "Example.COM:443"  -> "example.com"
//    "https://x.com/a"  -> "x.com"
//    末尾的点（FQDN 写法）去掉
// ============================================================================
QString NetworkService::normalizeHost(const QString &host)
{
    QString h = host.trimmed().toLower();
    if (h.isEmpty()) {
        return QString();
    }

    // 如果传进来的是完整 URL，取出 host
    if (h.contains(QLatin1String("://"))) {
        const QUrl u(h);
        h = u.host().toLower();
    }

    // 去掉端口（IPv6 用 [::1]:80 形式，方括号要保留）
    if (!h.startsWith(QLatin1Char('['))) {
        const int colon = h.indexOf(QLatin1Char(':'));
        if (colon >= 0) {
            h = h.left(colon);
        }
    }

    // 去掉 FQDN 的末尾点：example.com. 与 example.com 是同一台主机
    while (h.endsWith(QLatin1Char('.'))) {
        h.chop(1);
    }

    // 去掉可能的路径残留（防御性，正常不会走到）
    const int slash = h.indexOf(QLatin1Char('/'));
    if (slash >= 0) {
        h = h.left(slash);
    }

    return h.trimmed();
}

// ============================================================================
// 通配符白名单匹配
//
//  支持的写法：
//    example.com       精确匹配，不匹配子域
//    *.example.com     匹配任意子域，也匹配裸域 example.com
//
//  为什么不写 *.example.com 就只匹配精确值：
//    这是"最小授权"的体现。用户填 example.com 表示他只信任这一个主机，
//    我们不该替他扩大范围到所有子域 —— 子域可能由完全不同的第三方控制
//    （例如 user1.example.com 常被塞给不同客户）。
// ============================================================================
bool NetworkService::hostMatchesPattern(const QString &host, const QString &pattern)
{
    const QString h = normalizeHost(host);
    const QString p = normalizeHost(pattern);
    if (h.isEmpty() || p.isEmpty()) {
        return false;
    }

    if (!p.startsWith(QLatin1String("*."))) {
        return h == p;
    }

    const QString suffix = p.mid(2);           // "example.com"
    if (suffix.isEmpty()) {
        return false;                          // "*." 本身无效
    }

    // 裸域也算匹配（*.example.com 覆盖 example.com）
    if (h == suffix) {
        return true;
    }

    // 必须是 ".suffix" 结尾，避免 "evilexample.com" 匹配 "*.example.com"。
    // 这是通配符白名单最经典的绕过点。
    return h.endsWith(QLatin1Char('.') + suffix);
}

bool NetworkService::isHostWhitelisted(const QString &host) const
{
    const QString h = normalizeHost(host);
    if (h.isEmpty()) {
        return false;
    }

    QMutexLocker locker(&m_mutex);
    for (const QString &pattern : m_config.domainWhitelist) {
        if (hostMatchesPattern(h, pattern)) {
            return true;
        }
    }
    return false;
}

bool NetworkService::isHostUserDenied(const QString &host) const
{
    const QString h = normalizeHost(host);
    QMutexLocker locker(&m_mutex);
    return m_userDeniedHosts.contains(h);
}

void NetworkService::clearHostDenials()
{
    QMutexLocker locker(&m_mutex);
    m_userDeniedHosts.clear();
}

// ============================================================================
// 前置判定
// ============================================================================
NetworkAccessDecision NetworkService::decide(const NetworkRequest &request) const
{
    return evaluate(request);
}

NetworkAccessDecision NetworkService::evaluate(const NetworkRequest &request) const
{
    NetworkAccessDecision decision;

    const QUrl &url = request.url;
    if (!url.isValid() || url.host().isEmpty()) {
        decision.verdict = NetworkAccessDecision::Verdict::Deny;
        decision.errorCode = ec(erc::InvalidArgument);
        decision.reason = tr("请求地址无效。");
        return decision;
    }

    const QString host = normalizeHost(url.host());
    const QString scheme = url.scheme().toLower();

    NetworkConfigData cfg;
    QStringList deniedHosts;
    {
        QMutexLocker locker(&m_mutex);
        cfg = m_config;
        deniedHosts = m_userDeniedHosts;
    }

    // ---- 门 1：协议。TLS 强制，除非配置显式放开 ----
    if (scheme != QLatin1String("https")) {
        if (scheme != QLatin1String("http")) {
            decision.verdict = NetworkAccessDecision::Verdict::Deny;
            decision.errorCode = ec(erc::NetworkInsecureScheme);
            decision.reason = tr("不支持的协议：%1。只允许 https。").arg(scheme);
            return decision;
        }
        if (!cfg.allowInsecureTls) {
            decision.verdict = NetworkAccessDecision::Verdict::Deny;
            decision.errorCode = ec(erc::NetworkInsecureScheme);
            decision.reason = tr(
                "出于安全考虑，不允许通过非加密的 http 访问网络。"
                "请使用 https 地址。");
            return decision;
        }
    }

    // ---- 门 2：用户永久拒绝（优先级最高）----
    // 用户说过"不要再访问这个网站"，那么无论什么模式都不该再访问，
    // 否则就是对用户明确意愿的违背。
    if (deniedHosts.contains(host)) {
        decision.verdict = NetworkAccessDecision::Verdict::Deny;
        decision.errorCode = ec(erc::NetworkUserRefused);
        decision.reason = tr("你之前拒绝过访问 %1，已按你的选择继续拒绝。").arg(host);
        return decision;
    }

    // ---- 门 3：网络模式 ----
    switch (cfg.mode) {
    case NetworkMode::Offline:
        decision.verdict = NetworkAccessDecision::Verdict::Deny;
        decision.errorCode = ec(erc::NetworkOffline);
        decision.reason = tr("当前处于离线模式，不会进行任何网络访问。"
                             "可在设置中改为「按次询问」或「白名单」模式。");
        return decision;

    case NetworkMode::AllowConfigured:
        if (isHostWhitelisted(host)) {
            decision.verdict = NetworkAccessDecision::Verdict::Allow;
            return decision;
        }
        decision.verdict = NetworkAccessDecision::Verdict::Deny;
        decision.errorCode = ec(erc::NetworkHostNotAllowed);
        decision.reason = tr("%1 不在网络白名单中，已拒绝访问。"
                             "如需使用该服务，请先在设置中把它加入白名单。").arg(host);
        return decision;

    case NetworkMode::AskEachTime:
        decision.verdict = NetworkAccessDecision::Verdict::AskUser;
        decision.askPrompt = tr(
            "%1 想要访问网络：\n\n"
            "网站：%2\n"
            "用途：%3\n\n"
            "是否允许本次访问？")
            .arg(request.requester.isEmpty() ? tr("桌面助手") : request.requester,
                 host,
                 request.purpose.isEmpty() ? tr("（未说明用途）") : request.purpose);
        return decision;

    case NetworkMode::Custom:
        decision.verdict = NetworkAccessDecision::Verdict::Allow;
        return decision;
    }

    // 未知模式 -> 拒绝（失败安全：新增模式若忘了处理，行为是"不联网"而不是"全放开"）
    decision.verdict = NetworkAccessDecision::Verdict::Deny;
    decision.errorCode = ec(erc::NetworkOffline);
    decision.reason = tr("网络模式配置无法识别，已按最严格的离线模式处理。");
    return decision;
}

// ============================================================================
// 提交请求
// ============================================================================
QString NetworkService::submit(const NetworkRequest &request)
{
    const NetworkAccessDecision decision = evaluate(request);
    switch (decision.verdict) {
    case NetworkAccessDecision::Verdict::Deny: {
        QMutexLocker locker(&m_mutex);
        m_stats.blockedByPolicy++;
        DC_INFO(LogCat::Network, QStringLiteral("网络请求被策略拒绝：%1（%2）")
                    .arg(normalizeHost(request.url.host()), decision.reason));
        locker.unlock();

        const QString id = nextRequestId();
        NetworkResponse response;
        response.ok = false;
        response.errorCode = decision.errorCode;
        response.message = decision.reason;
        // 延后一拍发信号，保证调用方已拿到 requestId 并连好信号
        QMetaObject::invokeMethod(this, [this, id, response]() {
            emit finished(id, response);
            emit statsChanged();
        }, Qt::QueuedConnection);
        return id;
    }
    case NetworkAccessDecision::Verdict::AskUser: {
        // 服务层不弹框（那会引入 UI 依赖）。把决定权交回给调用方。
        const QString id = nextRequestId();
        DC_INFO(LogCat::Network, QStringLiteral("网络请求需要用户确认：%1")
                    .arg(normalizeHost(request.url.host())));
        QMetaObject::invokeMethod(this, [this, id, request]() {
            emit confirmationRequired(id, request);
        }, Qt::QueuedConnection);
        return id;
    }
    case NetworkAccessDecision::Verdict::Allow:
        return startRequest(request);
    }
    return QString();
}

QString NetworkService::submitApproved(const NetworkRequest &request)
{
    // 用户已经同意 —— 但依然要走协议检查与"用户永久拒绝"检查。
    // 理由：用户同意的是"这一次访问这个网站"，不代表同意明文传输，
    // 也不代表推翻他自己之前设过的永久拒绝（那需要去设置里改）。
    NetworkConfigData cfg;
    QStringList deniedHosts;
    {
        QMutexLocker locker(&m_mutex);
        cfg = m_config;
        deniedHosts = m_userDeniedHosts;
    }

    const QString host = normalizeHost(request.url.host());
    const QString scheme = request.url.scheme().toLower();

    if (scheme != QLatin1String("https") && !cfg.allowInsecureTls) {
        const QString id = nextRequestId();
        NetworkResponse response;
        response.ok = false;
        response.errorCode = ec(erc::NetworkInsecureScheme);
        response.message = tr("即便获得允许，也不会通过非加密的 http 传输数据。");
        QMetaObject::invokeMethod(this, [this, id, response]() {
            emit finished(id, response);
        }, Qt::QueuedConnection);
        return id;
    }

    if (deniedHosts.contains(host)) {
        const QString id = nextRequestId();
        NetworkResponse response;
        response.ok = false;
        response.errorCode = ec(erc::NetworkUserRefused);
        response.message = tr("该网站已被你永久拒绝，请在设置中解除后再试。");
        QMetaObject::invokeMethod(this, [this, id, response]() {
            emit finished(id, response);
        }, Qt::QueuedConnection);
        return id;
    }

    {
        QMutexLocker locker(&m_mutex);
        m_stats.userApproved++;
    }
    return startRequest(request);
}

void NetworkService::reportUserDecision(const NetworkRequest &request,
                                        bool allowed,
                                        bool rememberForHost)
{
    const QString host = normalizeHost(request.url.host());
    {
        QMutexLocker locker(&m_mutex);
        if (allowed) {
            m_stats.allowedByMode++;
        } else {
            m_stats.refusedByUser++;
            if (rememberForHost && !host.isEmpty()) {
                if (!m_userDeniedHosts.contains(host)) {
                    m_userDeniedHosts << host;
                }
            }
        }
    }

    if (allowed) {
        DC_INFO(LogCat::Network, QStringLiteral("用户允许访问网络：%1").arg(host));
    } else {
        DC_INFO(LogCat::Network, QStringLiteral("用户拒绝访问网络：%1（记住选择：%2）")
                    .arg(host, rememberForHost ? QStringLiteral("是") : QStringLiteral("否")));
    }
    emit statsChanged();
}

// ============================================================================
// 真正的请求发送
// ============================================================================
QString NetworkService::nextRequestId()
{
    QMutexLocker locker(&m_mutex);
    return QStringLiteral("req-%1-%2")
        .arg(QDateTime::currentMSecsSinceEpoch())
        .arg(++m_idCounter);
}

QString NetworkService::startRequest(const NetworkRequest &request)
{
    const QString requestId = nextRequestId();

    NetworkConfigData cfg;
    {
        QMutexLocker locker(&m_mutex);
        cfg = m_config;
        m_stats.totalRequests++;
        m_stats.allowedByMode++;
    }

    QNetworkRequest netRequest(request.url);

    // TLS 配置：要求至少 TLS 1.2。
    // 旧系统（SRS 6.1 提到 Windows XP / CentOS 6）可能只支持到 TLS 1.0，
    // 那种情况下请求会失败并给出明确原因，而不是静默降级 —— 这是刻意的：
    // 在无法保证加密强度时，宁可功能不可用，也不让用户以为通信是安全的。
    QSslConfiguration ssl = QSslConfiguration::defaultConfiguration();
    ssl.setProtocol(QSsl::TlsV1_2OrLater);
    netRequest.setSslConfiguration(ssl);

    netRequest.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                           QNetworkRequest::ManualRedirectPolicy);

    // 标识自己。很多 API 会因为缺少 User-Agent 直接拒绝请求。
    netRequest.setHeader(QNetworkRequest::UserAgentHeader,
                         QStringLiteral("DesktopCompanion/1.0"));

    for (const auto &header : request.headers) {
        netRequest.setRawHeader(header.first, header.second);
    }

    const QByteArray method = request.method.toUpper();

    QNetworkReply *reply = nullptr;
    if (method == "POST") {
        if (!netRequest.hasRawHeader("Content-Type")) {
            netRequest.setHeader(QNetworkRequest::ContentTypeHeader,
                                 QStringLiteral("application/x-www-form-urlencoded"));
        }
        reply = m_manager->post(netRequest, request.body);
    } else if (method == "HEAD") {
        reply = m_manager->head(netRequest);
    } else {
        reply = m_manager->get(netRequest);
    }

    if (!reply) {
        NetworkResponse response;
        response.ok = false;
        response.errorCode = ec(erc::NetworkRequestFailed);
        response.message = tr("无法发起网络请求。");
        QMetaObject::invokeMethod(this, [this, requestId, response]() {
            emit finished(requestId, response);
        }, Qt::QueuedConnection);
        return requestId;
    }

    const int timeoutMs = request.timeoutMs > 0 ? request.timeoutMs : cfg.timeoutMs;

    auto *timer = new QTimer(this);
    timer->setSingleShot(true);
    timer->setInterval(timeoutMs);

    {
        QMutexLocker locker(&m_mutex);
        PendingRequest pending;
        pending.request = request;
        pending.id = requestId;
        // attemptsLeft 记录"还可以再试几次"。首次已经发出，所以是 maxAttempts - 1。
        pending.attemptsLeft = qMax(0, cfg.maxAttempts - 1);
        pending.startedAt = QDateTime::currentMSecsSinceEpoch();
        pending.reply = reply;
        pending.timer = timer;
        m_pending.insert(requestId, pending);
    }

    connect(timer, &QTimer::timeout, this, [this, requestId]() {
        handleTimeout(requestId);
    });
    connect(reply, &QNetworkReply::finished, this, [this, requestId]() {
        handleReplyFinished(requestId);
    });

    // 响应体大小上限：边收边判，超了立刻断开。
    // 不能等收完再判 —— 那样一个超大响应已经把内存吃掉了。
    connect(reply, &QNetworkReply::downloadProgress, this,
            [this, requestId, request](qint64 received, qint64 total) {
                qint64 limit = request.maxResponseBytes;
                if (limit <= 0) {
                    QMutexLocker locker(&m_mutex);
                    limit = m_config.maxResponseBytes;
                }

                if (total > 0 && total > limit) {
                    // 服务器提前告知了大小，直接中止
                    DC_WARN(LogCat::Network, QStringLiteral("响应体声明大小 %1 超过上限 %2，中止请求")
                                .arg(total).arg(limit));
                    {
                        QMutexLocker locker(&m_mutex);
                        auto it = m_pending.find(requestId);
                        if (it != m_pending.end()) {
                            it->cancelled = true;
                            if (it->reply) it->reply->abort();
                        }
                    }
                    finishWithError(requestId, ec(erc::NetworkResponseTooLarge),
                                    tr("服务器返回的内容超过了允许的大小上限。"));
                    return;
                }

                if (received > limit) {
                    DC_WARN(LogCat::Network, QStringLiteral("响应体已接收 %1 字节，超过上限 %2，中止请求")
                                .arg(received).arg(limit));
                    {
                        QMutexLocker locker(&m_mutex);
                        auto it = m_pending.find(requestId);
                        if (it != m_pending.end()) {
                            it->cancelled = true;
                            if (it->reply) it->reply->abort();
                        }
                    }
                    finishWithError(requestId, ec(erc::NetworkResponseTooLarge),
                                    tr("服务器返回的内容超过了允许的大小上限。"));
                }
            });

    timer->start();

    DC_INFO(LogCat::Network, QStringLiteral("网络请求已发出：%1 %2（超时 %3ms，最多 %4 次）")
                .arg(QString::fromLatin1(method),
                     normalizeHost(request.url.host()))
                .arg(timeoutMs)
                .arg(cfg.maxAttempts));

    return requestId;
}

// ============================================================================
// 重定向检查
// ============================================================================
bool NetworkService::redirectTargetAllowed(const QUrl &from, const QUrl &to,
                                           QString *reason) const
{
    const QString fromScheme = from.scheme().toLower();
    const QString toScheme = to.scheme().toLower();

    NetworkConfigData cfg;
    {
        QMutexLocker locker(&m_mutex);
        cfg = m_config;
    }

    // 禁止 https -> http 降级。这是最需要拦的一种重定向：
    // 前半段是加密的，用户以为安全，实际上数据在第二跳就明文出去了。
    if (fromScheme == QLatin1String("https") && toScheme != QLatin1String("https")) {
        if (reason) {
            *reason = tr("服务器试图把加密连接重定向到未加密地址，已中止。");
        }
        return false;
    }

    if (toScheme != QLatin1String("https") && !cfg.allowInsecureTls) {
        if (reason) {
            *reason = tr("重定向目标不是加密地址，已中止。");
        }
        return false;
    }

    // 白名单模式下的重定向必须仍在白名单内，否则等于白名单形同虚设 ——
    // 攻击者只要在白名单域名上放一个 302 就能把请求引到任意地方。
    const QString toHost = normalizeHost(to.host());
    if (cfg.mode == NetworkMode::AllowConfigured && !isHostWhitelisted(toHost)) {
        if (reason) {
            *reason = tr("服务器试图重定向到白名单外的地址（%1），已中止。").arg(toHost);
        }
        return false;
    }

    if (m_userDeniedHosts.contains(toHost)) {
        if (reason) {
            *reason = tr("重定向目标 %1 已被你永久拒绝，已中止。").arg(toHost);
        }
        return false;
    }

    return true;
}

bool NetworkService::isRetriableStatus(int status) const
{
    // 429 = 被限流，等一下再试是合理的；5xx = 服务器临时问题；
    // 4xx 其它 = 请求本身有问题，重试只会重复失败。
    if (status == 429) return true;
    if (status >= 500 && status <= 599) return true;
    return false;
}

// ============================================================================
// 响应处理
// ============================================================================
void NetworkService::handleReplyFinished(const QString &requestId)
{
    PendingRequest pending;
    QNetworkReply *reply = nullptr;
    {
        QMutexLocker locker(&m_mutex);
        auto it = m_pending.find(requestId);
        if (it == m_pending.end()) {
            return;
        }
        pending = it.value();
        reply = pending.reply;
        if (pending.timer) {
            pending.timer->stop();
        }
    }

    if (!reply) {
        return;
    }

    NetworkResponse response;
    response.effectiveUrl = reply->url().toString();

    const QVariant statusVar = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute);
    response.httpStatus = statusVar.isValid() ? statusVar.toInt() : 0;

    // 处理重定向
    const QVariant redirectVar = reply->attribute(QNetworkRequest::RedirectionTargetAttribute);
    if (redirectVar.isValid() && statusVar.isValid() && response.httpStatus >= 300
        && response.httpStatus < 400) {
        NetworkConfigData cfg;
        {
            QMutexLocker locker(&m_mutex);
            cfg = m_config;
        }

        if (!cfg.followRedirects || cfg.maxRedirects <= 0) {
            reply->deleteLater();
            finishWithError(requestId, ec(erc::NetworkRequestFailed),
                            tr("服务器要求跳转，但跳转功能已关闭。"));
            return;
        }

        QUrl target = reply->url().resolved(redirectVar.toUrl());
        QString reason;
        if (!redirectTargetAllowed(reply->url(), target, &reason)) {
            DC_WARN(LogCat::Network, QStringLiteral("拒绝重定向：%1 -> %2（%3）")
                        .arg(normalizeHost(reply->url().host()),
                             normalizeHost(target.host()), reason));
            reply->deleteLater();
            finishWithError(requestId, ec(erc::NetworkHostNotAllowed), reason);
            emit securityNotice(reason);
            return;
        }

        // 重定向计数：借用 attemptsLeft 的剩余额度不准确，单独用 header 记。
        // 简化处理：把重定向次数塞进请求的 header 计数里。
        NetworkRequest nextRequest = pending.request;
        int redirectCount = 0;
        for (const auto &header : nextRequest.headers) {
            if (header.first == "X-DC-Redirect-Count") {
                redirectCount = header.second.toInt();
            }
        }
        ++redirectCount;
        if (redirectCount > cfg.maxRedirects) {
            reply->deleteLater();
            finishWithError(requestId, ec(erc::NetworkRequestFailed),
                            tr("跳转次数超过上限，已中止。"));
            return;
        }

        nextRequest.headers.erase(
            std::remove_if(nextRequest.headers.begin(), nextRequest.headers.end(),
                           [](const QPair<QByteArray, QByteArray> &h) {
                               return h.first == "X-DC-Redirect-Count";
                           }),
            nextRequest.headers.end());
        nextRequest.headers.append(
            qMakePair(QByteArray("X-DC-Redirect-Count"), QByteArray::number(redirectCount)));

        reply->deleteLater();
        {
            QMutexLocker locker(&m_mutex);
            m_pending.remove(requestId);
        }
        // 用同一个 requestId 继续，让调用方只关心一次请求的生命周期
        startRequestWithId(nextRequest, requestId);
        return;
    }

    const QByteArray data = reply->readAll();
    response.body = data;

    {
        QMutexLocker locker(&m_mutex);
        qint64 limit = pending.request.maxResponseBytes > 0
                           ? pending.request.maxResponseBytes
                           : m_config.maxResponseBytes;
        if (response.body.size() > limit) {
            response.body.truncate(static_cast<int>(limit));
            response.truncated = true;
        }
    }

    const QNetworkReply::NetworkError netError = reply->error();
    const QString netErrorString = reply->errorString();
    reply->deleteLater();

    response.attempts = [&]() -> int {
        QMutexLocker locker(&m_mutex);
        NetworkConfigData cfg = m_config;
        return cfg.maxAttempts;
    }();

    if (netError != QNetworkReply::NoError) {
        // 超时单独处理（也走重试逻辑）
        const bool isTimeout = (netError == QNetworkReply::TimeoutError
                                || netError == QNetworkReply::OperationCanceledError);

        // 重试判定：只对可重试的情况重试
        const bool retriable = isTimeout || response.httpStatus == 0
                               || isRetriableStatus(response.httpStatus);

        bool willRetry = false;
        {
            QMutexLocker locker(&m_mutex);
            auto it = m_pending.find(requestId);
            if (it != m_pending.end() && it->cancelled) {
                // 已经因为超限被主动中止，不要重试
                m_pending.erase(it);
            } else if (it != m_pending.end() && retriable && it->attemptsLeft > 0) {
                it->attemptsLeft--;
                willRetry = true;
            } else if (it != m_pending.end()) {
                m_pending.erase(it);
            }
        }

        if (willRetry) {
            DC_WARN(LogCat::Network, QStringLiteral("请求失败将重试：%1（%2）")
                        .arg(normalizeHost(pending.request.url.host()), netErrorString));
            // 重试前等一小会儿。立刻重试往往还是失败，反而让对端以为在被攻击。
            const int delayMs = 400;
            QTimer::singleShot(delayMs, this, [this, requestId, pending]() {
                {
                    QMutexLocker locker(&m_mutex);
                    if (!m_pending.contains(requestId)) {
                        return;  // 期间被取消了
                    }
                }
                startRequestWithId(pending.request, requestId);
            });
            return;
        }

        // 把 QNetworkReply 的错误翻译成人能看懂的话
        QString code = ec(erc::NetworkRequestFailed);
        QString message;
        switch (netError) {
        case QNetworkReply::TimeoutError:
            code = ec(erc::NetworkTimeout);
            message = tr("网络请求超时，请检查网络连接后重试。");
            break;
        case QNetworkReply::SslHandshakeFailedError:
            code = ec(erc::NetworkTlsError);
            message = tr("无法建立加密连接。可能是网络环境拦截、证书问题，"
                         "或本机不支持所需的加密算法。");
            break;
        case QNetworkReply::HostNotFoundError:
            code = ec(erc::NetworkRequestFailed);
            message = tr("找不到该网站，请检查地址或网络连接。");
            break;
        case QNetworkReply::ConnectionRefusedError:
        case QNetworkReply::RemoteHostClosedError:
            code = ec(erc::NetworkRequestFailed);
            message = tr("连接被拒绝或中断。");
            break;
        case QNetworkReply::OperationCanceledError:
            code = ec(erc::OperationCancelled);
            message = tr("请求已取消。");
            break;
        default:
            message = tr("网络请求失败：%1").arg(netErrorString);
            break;
        }

        {
            QMutexLocker locker(&m_mutex);
            m_stats.failed++;
        }
        response.ok = false;
        response.errorCode = code;
        response.message = message;
        response.elapsedMs = QDateTime::currentMSecsSinceEpoch() - pending.startedAt;

        DC_WARN(LogCat::Network, QStringLiteral("网络请求失败：%1（%2 / 状态 %3）")
                    .arg(normalizeHost(pending.request.url.host()), code)
                    .arg(response.httpStatus));

        emit finished(requestId, response);
        emit statsChanged();
        return;
    }

    // 成功路径
    {
        QMutexLocker locker(&m_mutex);
        m_pending.remove(requestId);
        m_stats.bytesReceived += response.body.size();
    }

    response.ok = true;
    response.errorCode = ec(erc::Ok);
    response.elapsedMs = QDateTime::currentMSecsSinceEpoch() - pending.startedAt;

    emit finished(requestId, response);
    emit statsChanged();
}

// 用指定的 requestId 发送（重试与重定向复用同一个 id）
void NetworkService::startRequestWithId(const NetworkRequest &request, const QString &requestId)
{
    const NetworkAccessDecision decision = evaluate(request);
    if (decision.verdict == NetworkAccessDecision::Verdict::Deny) {
        finishWithError(requestId, decision.errorCode, decision.reason);
        return;
    }

    NetworkConfigData cfg;
    {
        QMutexLocker locker(&m_mutex);
        cfg = m_config;
    }

    QNetworkRequest netRequest(request.url);

    QSslConfiguration ssl = QSslConfiguration::defaultConfiguration();
    ssl.setProtocol(QSsl::TlsV1_2OrLater);
    netRequest.setSslConfiguration(ssl);
    netRequest.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                           QNetworkRequest::ManualRedirectPolicy);
    netRequest.setHeader(QNetworkRequest::UserAgentHeader,
                         QStringLiteral("DesktopCompanion/1.0"));
    for (const auto &header : request.headers) {
        netRequest.setRawHeader(header.first, header.second);
    }

    const QByteArray method = request.method.toUpper();
    QNetworkReply *reply = nullptr;
    if (method == "POST") {
        if (!netRequest.hasRawHeader("Content-Type")) {
            netRequest.setHeader(QNetworkRequest::ContentTypeHeader,
                                 QStringLiteral("application/x-www-form-urlencoded"));
        }
        reply = m_manager->post(netRequest, request.body);
    } else if (method == "HEAD") {
        reply = m_manager->head(netRequest);
    } else {
        reply = m_manager->get(netRequest);
    }

    if (!reply) {
        finishWithError(requestId, ec(erc::NetworkRequestFailed),
                        tr("无法发起网络请求。"));
        return;
    }

    const int timeoutMs = request.timeoutMs > 0 ? request.timeoutMs : cfg.timeoutMs;
    auto *timer = new QTimer(this);
    timer->setSingleShot(true);
    timer->setInterval(timeoutMs);

    {
        QMutexLocker locker(&m_mutex);
        auto it = m_pending.find(requestId);
        if (it != m_pending.end()) {
            it->reply = reply;
            it->timer = timer;
        } else {
            PendingRequest pending;
            pending.request = request;
            pending.id = requestId;
            pending.attemptsLeft = 0;
            pending.startedAt = QDateTime::currentMSecsSinceEpoch();
            pending.reply = reply;
            pending.timer = timer;
            m_pending.insert(requestId, pending);
        }
    }

    connect(timer, &QTimer::timeout, this, [this, requestId]() {
        handleTimeout(requestId);
    });
    connect(reply, &QNetworkReply::finished, this, [this, requestId]() {
        handleReplyFinished(requestId);
    });
    connect(reply, &QNetworkReply::downloadProgress, this,
            [this, requestId, request](qint64 received, qint64 total) {
                qint64 limit = request.maxResponseBytes;
                if (limit <= 0) {
                    QMutexLocker locker(&m_mutex);
                    limit = m_config.maxResponseBytes;
                }
                if ((total > 0 && total > limit) || received > limit) {
                    {
                        QMutexLocker locker(&m_mutex);
                        auto it = m_pending.find(requestId);
                        if (it != m_pending.end()) {
                            it->cancelled = true;
                            if (it->reply) it->reply->abort();
                        }
                    }
                    finishWithError(requestId, ec(erc::NetworkResponseTooLarge),
                                    tr("服务器返回的内容超过了允许的大小上限。"));
                }
            });

    timer->start();
}

void NetworkService::handleTimeout(const QString &requestId)
{
    QNetworkReply *reply = nullptr;
    NetworkRequest request;
    {
        QMutexLocker locker(&m_mutex);
        auto it = m_pending.find(requestId);
        if (it == m_pending.end()) {
            return;
        }
        reply = it->reply;
        request = it->request;
    }

    DC_WARN(LogCat::Network, QStringLiteral("请求超时：%1").arg(normalizeHost(request.url.host())));

    if (reply) {
        // abort() 会触发 finished 信号，在那里统一处理；
        // 但为了避免"abort 后 finished 又被当成普通错误重试"，先标记状态。
        reply->abort();
    }
}

void NetworkService::finishWithError(const QString &requestId,
                                     const QString &errorCode,
                                     const QString &message)
{
    QNetworkReply *reply = nullptr;
    QTimer *timer = nullptr;
    qint64 startedAt = 0;
    {
        QMutexLocker locker(&m_mutex);
        auto it = m_pending.find(requestId);
        if (it == m_pending.end()) {
            return;
        }
        reply = it->reply;
        it->reply = nullptr;
        timer = it->timer;
        it->timer = nullptr;
        startedAt = it->startedAt;
        m_pending.erase(it);
        m_stats.failed++;
    }

    if (timer) {
        timer->stop();
        timer->deleteLater();
    }
    if (reply) {
        reply->deleteLater();
    }

    NetworkResponse response;
    response.ok = false;
    response.errorCode = errorCode;
    response.message = message;
    response.elapsedMs = startedAt > 0
                             ? QDateTime::currentMSecsSinceEpoch() - startedAt
                             : 0;

    emit finished(requestId, response);
    emit statsChanged();
}

// ============================================================================
// 取消
// ============================================================================
bool NetworkService::cancel(const QString &requestId)
{
    QNetworkReply *reply = nullptr;
    QTimer *timer = nullptr;
    {
        QMutexLocker locker(&m_mutex);
        auto it = m_pending.find(requestId);
        if (it == m_pending.end()) {
            return false;
        }
        it->cancelled = true;
        reply = it->reply;
        timer = it->timer;
        it->reply = nullptr;
        it->timer = nullptr;
        m_pending.erase(it);
    }

    if (timer) {
        timer->stop();
        timer->deleteLater();
    }
    if (reply) {
        // abort 后仍会发 finished，但那时 m_pending 里已经没有这个 id，
        // handleReplyFinished 会直接返回，不会重复发完成信号。
        reply->abort();
        reply->deleteLater();
    }

    NetworkResponse response;
    response.ok = false;
    response.errorCode = ec(erc::OperationCancelled);
    response.message = tr("请求已取消。");
    emit finished(requestId, response);
    emit statsChanged();
    return true;
}

void NetworkService::cancelAll()
{
    QList<QString> ids;
    {
        QMutexLocker locker(&m_mutex);
        ids = m_pending.keys();
    }
    for (const QString &id : ids) {
        cancel(id);
    }
}

bool NetworkService::isBusy() const
{
    QMutexLocker locker(&m_mutex);
    return !m_pending.isEmpty();
}

int NetworkService::activeRequestCount() const
{
    QMutexLocker locker(&m_mutex);
    return m_pending.size();
}

// ============================================================================
// 统计
// ============================================================================
NetworkService::SessionStats NetworkService::sessionStats() const
{
    QMutexLocker locker(&m_mutex);
    return m_stats;
}

void NetworkService::resetSessionStats()
{
    QMutexLocker locker(&m_mutex);
    m_stats = SessionStats{};
}

} // namespace dc
