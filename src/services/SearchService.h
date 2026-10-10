#pragma once

// ============================================================================
//  SearchService —— 网络搜索（SRS 4.7 / 4.9）
//
//  需求原文（SRS 4.9 搜索与翻译）：
//    - 搜索：支持通过搜索引擎发起查询，结果在默认浏览器中打开
//    - 翻译：支持文本翻译，可替换翻译后端
//
//  设计说明（为什么搜索是"打开浏览器"而不是"抓取结果页"）：
//    抓取搜索引擎结果页有两个问题：
//      1. 违反搜索引擎的使用条款，且极易因页面结构变化而失效；
//      2. 需要把用户查询发给第三方并解析其 HTML，攻击面大。
//    因此本实现的做法是：组合出标准的搜索 URL，交给默认浏览器打开。
//    这样用户能看到完整的搜索结果页、能登录自己的账号、能翻页 —— 
//    体验更好，且我们不需要联网抓取任何内容。
//
//    这也是为什么 SearchService 默认甚至不需要网络权限：
//    它只调用 SystemService::openUrl()，由操作系统去打开浏览器。
//    "打开浏览器搜索"这个动作本身不消耗本程序的网络授权。
//
//  但如果用户配置了"抓取式搜索"（用于本地缓存/历史），
//  则必须走 NetworkService，并且会受网络模式与白名单约束。
// ============================================================================

#include <QObject>
#include <QString>
#include <QStringList>
#include <QList>
#include <QUrl>
#include <QHash>

namespace dc {

class NetworkService;
class SystemService;

// ---------------------------------------------------------------------------
// 搜索引擎定义
// ---------------------------------------------------------------------------
struct SearchProvider {
    QString id;                 // "bing" / "google" / "baidu" / "duckduckgo" / "custom"
    QString displayName;        // 界面上显示的名字
    QString urlTemplate;        // 含 {query} 占位符
    QString homePage;           // 用于"打开首页"
    QString region;             // 适用地区提示（"cn" / "global" / 空）
    QString note;               // 给用户看的说明（例如隐私提示）
    bool enabled = true;

    // 该引擎是否要求联网抓取（本实现全部为 false）
    bool requiresNetworkFetch = false;
};

// ---------------------------------------------------------------------------
// 搜索结果条目（仅在启用"抓取式搜索"时有值）
// ---------------------------------------------------------------------------
struct SearchResultItem {
    QString title;
    QString url;
    QString snippet;
};

// ---------------------------------------------------------------------------
// 搜索请求
// ---------------------------------------------------------------------------
struct SearchRequest {
    QString query;
    QString providerId;          // 空 = 用配置里的默认引擎
    QString language;            // "zh" / "en" / "ja"，用于选择适合的引擎
    bool appendSiteFilter = false;
    QString siteFilter;
};

// ---------------------------------------------------------------------------
// 搜索执行结果
// ---------------------------------------------------------------------------
struct SearchOutcome {
    bool ok = false;
    QString errorCode;
    QString message;             // 面向用户
    QString openedUrl;           // 实际打开的地址（可展示给用户核对）
    QString providerId;
    bool openedInBrowser = false;

    // 仅抓取式搜索填充
    QList<SearchResultItem> items;
};

// ============================================================================
// SearchService
// ============================================================================
class SearchService : public QObject
{
    Q_OBJECT

public:
    explicit SearchService(QObject *parent = nullptr);
    ~SearchService() override;

    // 依赖注入（由 CoreRuntime 装配）。
    // 注意：SystemService 是必需依赖，NetworkService 仅在抓取式搜索时需要。
    void setSystemService(SystemService *systemService);
    void setNetworkService(NetworkService *networkService);

    // ---- 引擎管理 ----

    // 注册/覆盖一个引擎
    void registerProvider(const SearchProvider &provider);

    // 内置引擎（不联网即可用，纯 URL 组合）
    static QList<SearchProvider> builtinProviders();

    // 已注册的全部引擎
    QList<SearchProvider> providers() const;
    QStringList providerIds() const;

    // 查一个引擎
    SearchProvider provider(const QString &id) const;
    bool hasProvider(const QString &id) const;

    // 默认引擎
    void setDefaultProvider(const QString &id);
    QString defaultProviderId() const;

    // 按语言挑一个合适的默认引擎（用户没指定时用）
    QString suggestProviderForLanguage(const QString &language) const;

    // 禁用/启用
    void setProviderEnabled(const QString &id, bool enabled);

    // ---- 搜索 ----

    // 执行搜索：组合 URL 并用默认浏览器打开。
    // 这是默认且推荐的方式（不抓取、不消耗网络授权）。
    SearchOutcome searchInBrowser(const SearchRequest &request);

    // 只组合 URL，不打开。便于"复制链接"这类功能。
    SearchOutcome buildSearchUrl(const SearchRequest &request) const;

    // 抓取式搜索（可选功能，需要网络授权）。
    // 仅在用户显式启用、且网络模式允许时可用。
    QString searchAndFetch(const SearchRequest &request);

    // ---- 自定义引擎 ----

    // 校验用户输入的自定义 URL 模板是否可用。
    // 必须含 {query}，且协议必须是 https（除非显式允许明文）。
    static QString validateCustomTemplate(const QString &urlTemplate,
                                         bool allowInsecure);

    // 生成一个安全的模板示例，用于设置页的占位提示
    static QString templateExample();

    // ---- 编码 ----

    // 对查询词做 URL 编码。
    // 为什么不直接用 QUrlQuery：它会把空格编码成 '+'，而部分引擎只认 %20。
    // 这里统一用 %20，兼容性更好。
    static QString encodeQuery(const QString &query);

    // 从原始查询词里提取出安全的搜索关键词（去掉控制字符、限长）
    static QString sanitizeQuery(const QString &query);

signals:
    // 抓取式搜索完成
    void fetchFinished(const QString &requestId, const dc::SearchOutcome &outcome);

    // 搜索被打开（用于历史记录与统计）
    void searchOpened(const dc::SearchRequest &request, const QString &url);

private:
    // 组合最终 URL
    QUrl composeUrl(const SearchRequest &request, const SearchProvider &provider,
                    QString *errorMessage) const;

    SystemService *m_systemService = nullptr;
    NetworkService *m_networkService = nullptr;
    QHash<QString, SearchProvider> m_providers;
    QString m_defaultProviderId;
    quint64 m_fetchCounter = 0;
};

} // namespace dc

Q_DECLARE_METATYPE(dc::SearchOutcome)
Q_DECLARE_METATYPE(dc::SearchRequest)
