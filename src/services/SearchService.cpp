#include "SearchService.h"

#include "core/ErrorCode.h"
#include "core/Logger.h"
#include "services/NetworkService.h"
#include "services/SystemService.h"

#include <QRegularExpression>
#include <QUrlQuery>
#include <QHash>

#include <algorithm>

namespace dc {

static inline QString ec(const char *code)
{
    return QString::fromLatin1(code);
}

// ============================================================================
// 构造
// ============================================================================
SearchService::SearchService(QObject *parent)
    : QObject(parent)
{
    // 装配内置引擎
    const QList<SearchProvider> builtins = builtinProviders();
    for (const SearchProvider &p : builtins) {
        m_providers.insert(p.id, p);
    }

    // 默认引擎按 "地区可访问性 > 隐私友好度" 排序来选。
    // 这里默认 DuckDuckGo 是因为它不需要登录、不追踪，且在全球可访问。
    // 用户可以在设置里改成任何已注册的引擎。
    if (m_providers.contains(QStringLiteral("duckduckgo"))) {
        m_defaultProviderId = QStringLiteral("duckduckgo");
    } else if (!builtins.isEmpty()) {
        m_defaultProviderId = builtins.first().id;
    }
}

SearchService::~SearchService() = default;

void SearchService::setSystemService(SystemService *systemService)
{
    m_systemService = systemService;
}

void SearchService::setNetworkService(NetworkService *networkService)
{
    m_networkService = networkService;
}

// ============================================================================
// 内置引擎
//
//  选这几个的理由：
//    - 覆盖中英文主要使用场景，且都不需要 API Key
//    - URL 模板稳定（很多小引擎的搜索路径改过好几次）
//    - 都支持 https
//
//  注意 urlTemplate 里的 {query} 会被替换成已编码的查询词。
//  模板本身必须含 {query}，否则 validateCustomTemplate 会拒绝。
// ============================================================================
QList<SearchProvider> SearchService::builtinProviders()
{
    QList<SearchProvider> list;

    {
        SearchProvider p;
        p.id = QStringLiteral("duckduckgo");
        p.displayName = QStringLiteral("DuckDuckGo");
        p.urlTemplate = QStringLiteral("https://duckduckgo.com/?q={query}");
        p.homePage = QStringLiteral("https://duckduckgo.com/");
        p.region = QStringLiteral("global");
        p.note = QObject::tr("不追踪搜索记录，无需登录。");
        list << p;
    }
    {
        SearchProvider p;
        p.id = QStringLiteral("bing");
        p.displayName = QStringLiteral("Bing");
        p.urlTemplate = QStringLiteral("https://www.bing.com/search?q={query}");
        p.homePage = QStringLiteral("https://www.bing.com/");
        p.region = QStringLiteral("global");
        p.note = QObject::tr("国际版搜索，中文结果也较完整。");
        list << p;
    }
    {
        SearchProvider p;
        p.id = QStringLiteral("baidu");
        p.displayName = QStringLiteral("百度");
        p.urlTemplate = QStringLiteral("https://www.baidu.com/s?wd={query}");
        p.homePage = QStringLiteral("https://www.baidu.com/");
        p.region = QStringLiteral("cn");
        p.note = QObject::tr("中文内容覆盖最广，但会记录搜索历史。");
        list << p;
    }
    {
        SearchProvider p;
        p.id = QStringLiteral("google");
        p.displayName = QStringLiteral("Google");
        p.urlTemplate = QStringLiteral("https://www.google.com/search?q={query}");
        p.homePage = QStringLiteral("https://www.google.com/");
        p.region = QStringLiteral("global");
        p.note = QObject::tr("部分网络环境下可能无法访问。");
        list << p;
    }

    return list;
}

// ============================================================================
// 引擎管理
// ============================================================================
void SearchService::registerProvider(const SearchProvider &provider)
{
    if (provider.id.trimmed().isEmpty()) {
        DC_WARN(LogCat::Search, QStringLiteral("尝试注册 id 为空的搜索引擎，已忽略"));
        return;
    }
    if (provider.urlTemplate.trimmed().isEmpty()
        || !provider.urlTemplate.contains(QLatin1String("{query}"))) {
        DC_WARN(LogCat::Search, QStringLiteral("搜索引擎 %1 的 URL 模板缺少 {query} 占位符，已忽略")
                    .arg(provider.id));
        return;
    }
    m_providers.insert(provider.id, provider);
}

QList<SearchProvider> SearchService::providers() const
{
    QList<SearchProvider> list = m_providers.values();
    std::sort(list.begin(), list.end(),
              [](const SearchProvider &a, const SearchProvider &b) {
                  // 启用的排前面，再按显示名排序，保证界面顺序稳定
                  if (a.enabled != b.enabled) {
                      return a.enabled;
                  }
                  return a.displayName < b.displayName;
              });
    return list;
}

QStringList SearchService::providerIds() const
{
    QStringList ids = m_providers.keys();
    ids.sort();
    return ids;
}

SearchProvider SearchService::provider(const QString &id) const
{
    return m_providers.value(id);
}

bool SearchService::hasProvider(const QString &id) const
{
    return m_providers.contains(id);
}

void SearchService::setDefaultProvider(const QString &id)
{
    if (id.isEmpty() || m_providers.contains(id)) {
        m_defaultProviderId = id;
    } else {
        DC_WARN(LogCat::Search, QStringLiteral("尝试把不存在的搜索引擎设为默认：%1").arg(id));
    }
}

QString SearchService::defaultProviderId() const
{
    return m_defaultProviderId;
}

QString SearchService::suggestProviderForLanguage(const QString &language) const
{
    const QString lang = language.trimmed().toLower();

    // 中文用户默认给百度（覆盖好），但仅当它被启用；
    // 否则退回默认引擎，而不是硬塞一个不可用的。
    if (lang == QLatin1String("zh") || lang == QLatin1String("chinese")) {
        if (m_providers.contains(QStringLiteral("baidu"))
            && m_providers.value(QStringLiteral("baidu")).enabled) {
            return QStringLiteral("baidu");
        }
    }

    if (!m_defaultProviderId.isEmpty() && m_providers.contains(m_defaultProviderId)) {
        return m_defaultProviderId;
    }

    // 兜底：第一个启用的引擎
    const QList<SearchProvider> all = providers();
    for (const SearchProvider &p : all) {
        if (p.enabled) {
            return p.id;
        }
    }
    return QString();
}

void SearchService::setProviderEnabled(const QString &id, bool enabled)
{
    auto it = m_providers.find(id);
    if (it != m_providers.end()) {
        it->enabled = enabled;
    }
}

// ============================================================================
// 查询编码
//
//  为什么不用 QUrlQuery::addQueryItem：
//    它按 application/x-www-form-urlencoded 规则把空格编码成 '+'。
//    多数搜索引擎能接受，但少数会把 '+' 当成字面字符，
//    结果就是"搜 'c++ 教程' 搜不出来"这种诡异问题。
//    这里手工编码，空格统一用 %20，兼容性最好。
// ============================================================================
QString SearchService::encodeQuery(const QString &query)
{
    // QUrl::toPercentEncoding 默认不编码 "/" 和 ":"，在查询串里这两个字符
    // 会改变语义（例如 baidu 的 wd=http://x 可能被解析成带协议的东西），
    // 所以把它们也纳入编码范围。
    return QString::fromLatin1(
        QUrl::toPercentEncoding(query, QByteArray(), QByteArray("/:?#[]@!$&'()*+,;=")));
}

QString SearchService::sanitizeQuery(const QString &query)
{
    QString q = query;

    // 去掉控制字符（含 NUL、换行、各方向控制符）。
    // 换行尤其重要：某些引擎的 URL 参数带换行会被拼出奇怪的请求。
    QString cleaned;
    cleaned.reserve(q.size());
    for (const QChar &ch : q) {
        const ushort u = ch.unicode();
        if (u < 0x20 || u == 0x7F) {
            continue;
        }
        cleaned.append(ch);
    }
    q = cleaned;

    // 折叠连续空白，并去掉首尾空白
    q = q.simplified();

    // 长度上限：搜索引擎对超长查询本来也会截断，
    // 而超长 URL 在部分旧浏览器上会失败。
    static constexpr int kMaxQueryLength = 512;
    if (q.size() > kMaxQueryLength) {
        q = q.left(kMaxQueryLength);
    }

    return q;
}

// ============================================================================
// 自定义模板校验
// ============================================================================
QString SearchService::validateCustomTemplate(const QString &urlTemplate,
                                               bool allowInsecure)
{
    const QString tmpl = urlTemplate.trimmed();

    if (tmpl.isEmpty()) {
        return QObject::tr("地址模板不能为空。");
    }

    if (!tmpl.contains(QLatin1String("{query}"))) {
        return QObject::tr("地址模板必须包含 {query} 占位符，"
                           "它会被替换成你要搜索的内容。");
    }

    // 用占位词构造一个真实 URL 再校验，比正则匹配可靠
    const QString probe = tmpl;
    const QString filled = QString(probe).replace(
        QStringLiteral("{query}"), QStringLiteral("test"));

    const QUrl parsed(filled, QUrl::StrictMode);
    if (!parsed.isValid() || parsed.host().isEmpty()) {
        return QObject::tr("地址模板不是一个有效的网址，请检查是否写全了协议和域名。");
    }

    const QString scheme = parsed.scheme().toLower();
    if (scheme != QLatin1String("https")) {
        if (scheme == QLatin1String("http") && allowInsecure) {
            // 用户显式允许明文，但仍要提示
            return QString();
        }
        return QObject::tr("地址模板必须使用 https。非加密的搜索地址会泄露你的查询内容。");
    }

    // 模板里只允许出现一次 {query}，多次出现容易出意料之外的结果
    if (tmpl.count(QLatin1String("{query}")) > 1) {
        return QObject::tr("地址模板中只能出现一次 {query} 占位符。");
    }

    return QString();
}

QString SearchService::templateExample()
{
    return QStringLiteral("https://example.com/search?q={query}");
}

// ============================================================================
// URL 组合
// ============================================================================
QUrl SearchService::composeUrl(const SearchRequest &request,
                               const SearchProvider &provider,
                               QString *errorMessage) const
{
    const QString clean = sanitizeQuery(request.query);
    if (clean.isEmpty()) {
        if (errorMessage) {
            *errorMessage = tr("搜索内容不能为空。");
        }
        return QUrl();
    }

    QString templateStr = provider.urlTemplate;

    // 站点限定：把 site:example.com 拼到查询词里。
    // 这是通用做法，各引擎都支持 site: 语法。
    QString effectiveQuery = clean;
    if (request.appendSiteFilter && !request.siteFilter.trimmed().isEmpty()) {
        QString site = request.siteFilter.trimmed();
        // 站点值也要清洗，否则 "site:evil.com OR site:x" 这类会被注入
        site.remove(QLatin1Char(' '));
        site.remove(QLatin1Char('"'));
        site.remove(QLatin1Char('\''));
        if (!site.isEmpty()) {
            effectiveQuery = QStringLiteral("site:%1 %2").arg(site, clean);
        }
    }

    templateStr.replace(QStringLiteral("{query}"), encodeQuery(effectiveQuery));

    const QUrl url(templateStr, QUrl::StrictMode);
    if (!url.isValid() || url.host().isEmpty()) {
        if (errorMessage) {
            *errorMessage = tr("无法生成有效的搜索地址。");
        }
        return QUrl();
    }

    return url;
}

// ============================================================================
// buildSearchUrl
// ============================================================================
SearchOutcome SearchService::buildSearchUrl(const SearchRequest &request) const
{
    SearchOutcome outcome;

    const QString providerId = request.providerId.trimmed().isEmpty()
                                   ? m_defaultProviderId
                                   : request.providerId;

    if (!m_providers.contains(providerId)) {
        outcome.ok = false;
        outcome.errorCode = ec(erc::InvalidArgument);
        outcome.message = tr("未找到搜索引擎：%1").arg(providerId);
        return outcome;
    }

    const SearchProvider p = m_providers.value(providerId);
    if (!p.enabled) {
        outcome.ok = false;
        outcome.errorCode = ec(erc::InvalidArgument);
        outcome.message = tr("搜索引擎 %1 已被停用，请在设置中启用。").arg(p.displayName);
        return outcome;
    }

    QString error;
    const QUrl url = composeUrl(request, p, &error);
    if (!url.isValid()) {
        outcome.ok = false;
        outcome.errorCode = ec(erc::InvalidArgument);
        outcome.message = error;
        return outcome;
    }

    outcome.ok = true;
    outcome.errorCode = ec(erc::Ok);
    outcome.openedUrl = url.toString();
    outcome.providerId = providerId;
    return outcome;
}

// ============================================================================
// searchInBrowser —— 默认路径，不抓取
// ============================================================================
SearchOutcome SearchService::searchInBrowser(const SearchRequest &request)
{
    SearchOutcome outcome = buildSearchUrl(request);
    if (!outcome.ok) {
        return outcome;
    }

    if (!m_systemService) {
        outcome.ok = false;
        outcome.errorCode = ec(erc::NotInitialized);
        outcome.message = tr("系统服务未就绪，无法打开浏览器。");
        return outcome;
    }

    // 走 SystemService::openUrl，由它做协议校验（只允许 http/https）。
    // 这样即使模板被改成 file:// 之类，也会在这里被拦下。
    const SystemActionResult opened = m_systemService->openUrl(outcome.openedUrl);
    if (!opened.ok) {
        outcome.ok = false;
        outcome.errorCode = opened.errorCode;
        outcome.message = tr("无法打开浏览器：%1").arg(opened.message);
        return outcome;
    }

    outcome.openedInBrowser = true;

    DC_INFO(LogCat::Search, QStringLiteral("已用浏览器搜索：引擎=%1").arg(outcome.providerId));
    // 只记录"发生了搜索"，不记录查询词 —— 查询词属于用户隐私，
    // 是否记录由 HistoryService 的隐私开关决定，且由调用方显式写入。
    emit searchOpened(request, outcome.openedUrl);

    return outcome;
}

// ============================================================================
// 抓取式搜索（可选）
//
//  说明：本实现不内置 HTML 解析器（避免引入第三方依赖、避免被搜索引擎
//  反爬策略影响）。这里提供的是"按模板 URL 取回内容"的能力，
//  由调用方决定怎么用。默认界面不启用这条路径。
// ============================================================================
QString SearchService::searchAndFetch(const SearchRequest &request)
{
    const QString requestId = QStringLiteral("search-%1").arg(++m_fetchCounter);

    if (!m_networkService) {
        SearchOutcome outcome;
        outcome.ok = false;
        outcome.errorCode = ec(erc::NotInitialized);
        outcome.message = tr("网络服务未就绪，无法抓取搜索结果。");

        QMetaObject::invokeMethod(this, [this, requestId, outcome]() {
            emit fetchFinished(requestId, outcome);
        }, Qt::QueuedConnection);
        return requestId;
    }

    const SearchOutcome composed = buildSearchUrl(request);
    if (!composed.ok) {
        QMetaObject::invokeMethod(this, [this, requestId, composed]() {
            emit fetchFinished(requestId, composed);
        }, Qt::QueuedConnection);
        return requestId;
    }

    NetworkRequest netRequest;
    netRequest.requester = tr("搜索结果抓取");
    netRequest.url = QUrl(composed.openedUrl);
    netRequest.purpose = tr("获取搜索结果页内容");
    netRequest.expectText = true;
    // 抓取结果页通常比 API 响应大，给 2MB 上限
    netRequest.maxResponseBytes = 2 * 1024 * 1024;

    const QString netRequestId = m_networkService->submit(netRequest);

    // 把网络层的完成信号转成 SearchOutcome。
    // 用 ConnectOnce 式的临时连接，避免信号累积。
    auto *connection = new QMetaObject::Connection;
    *connection = connect(
        m_networkService, &NetworkService::finished, this,
        [this, connection, requestId, netRequestId, composed](
            const QString &finishedId, const NetworkResponse &response) {
            if (finishedId != netRequestId) {
                return;
            }
            disconnect(*connection);
            delete connection;

            SearchOutcome outcome = composed;
            if (!response.ok) {
                outcome.ok = false;
                outcome.errorCode = response.errorCode;
                outcome.message = response.message;
            } else {
                outcome.ok = true;
                outcome.errorCode = ec(erc::Ok);
                outcome.message.clear();
                // 注意：这里不解析 HTML。items 保持为空，
                // 由调用方决定是否需要进一步处理 raw 内容。
            }
            emit fetchFinished(requestId, outcome);
        });

    return requestId;
}

} // namespace dc
