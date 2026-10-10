// ============================================================================
//  CommandEngine.cpp — 命令解析实现
//
//  需求依据：SRS V1.1 3.2、4.2、4.6、7.3
// ============================================================================
#include "engine/CommandEngine.h"

#include "core/ErrorCode.h"
#include "core/Logger.h"
#include "engine/LanguageDetector.h"
#include "platform/PathGuard.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>

#include <algorithm>

namespace dc {

// ===========================================================================
//  构造
// ===========================================================================
CommandEngine::CommandEngine(CommandRepository *repository, QObject *parent)
    : QObject(parent)
    , m_repository(repository)
{
}

CommandEngine::~CommandEngine() = default;

void CommandEngine::setConfig(const CommandEngineConfig &config)
{
    m_config = config;
}

CommandEngineConfig CommandEngine::config() const
{
    return m_config;
}

// ===========================================================================
//  输入规范化
//
//  为什么需要全角转半角：
//    中文输入法下用户很容易打出全角字母（如 ＯＰＥＮ）和全角标点。
//    如果不归一化，规则里写 "open" 就匹配不上 "ＯＰＥＮ"，
//    用户会觉得「明明打对了却没反应」。
// ===========================================================================
QString CommandEngine::normalizeInput(const QString &text)
{
    QString out;
    out.reserve(text.size());

    for (const QChar &c : text) {
        const ushort u = c.unicode();

        // ---- 1) 剥离零宽与不可见控制字符（防注入/混淆）----
        //   U+200B 零宽空格, U+200C 零宽非连接符, U+200D 零宽连接符
        //   U+FEFF BOM, U+00AD 软连字符
        //   U+202A-U+202E 双向控制字符（可用于伪装文本）
        //   U+2066-U+2069 双向隔离字符
        if (u == 0x200B || u == 0x200C || u == 0x200D || u == 0xFEFF
            || u == 0x00AD
            || (u >= 0x202A && u <= 0x202E)
            || (u >= 0x2066 && u <= 0x2069)) {
            continue;
        }

        // ---- 2) 控制字符一律**删除**（不是替换为空格）----
        //  这里曾经写成「替换为空格」，是个真正的 bug：
        //    "打开\x01浏览器" 会被规范化成 "打开 浏览器"（中间多一个空格），
        //    而规则里写的是 "打开浏览器"，于是匹配不上 —— 用户看到的现象是
        //    「明明输入看起来是对的，桌宠却听不懂」。
        //
        //  更糟的是这会让「不可见字符」起到分词作用：攻击者插入 \x01 就能把
        //  一条命令拆成两段，从而绕过基于整串的模式匹配。
        //
        //  正确做法与上面第 1 步的零宽字符一致：直接丢弃。
        //  制表/换行（\t \n \r）是「合法空白」，单独处理为空格（见下面第 3 步），
        //  因为用户确实可能从别处粘贴多行文本。
        //
        //  覆盖面：C0 控制字符（0x00-0x1F）、DEL（0x7F）、
        //          C1 控制字符（0x80-0x9F）。
        //  只删 < 0x20 是不够的 —— 0x7F 与 C1 同样不可见、同样能起分词作用，
        //  「删一半」等于留了半扇门。
        if ((u < 0x20 && u != 0x09 && u != 0x0A && u != 0x0D)
            || u == 0x7F
            || (u >= 0x80 && u <= 0x9F)) {
            continue;
        }

        // ---- 3) 合法空白（制表/换行/回车）折叠为单个空格 ----
        //  多行粘贴时把换行当作词间分隔，而不是删掉 ——
        //  删掉会让 "打开\n浏览器" 变成 "打开浏览器"，扩大匹配面。
        if (u == 0x09 || u == 0x0A || u == 0x0D) {
            out.append(QLatin1Char(' '));
            continue;
        }

        // ---- 4) 全角 ASCII（U+FF01..U+FF5E）转半角 ----
        if (u >= 0xFF01 && u <= 0xFF5E) {
            out.append(QChar(u - 0xFEE0));
            continue;
        }
        // 全角空格
        if (u == 0x3000) {
            out.append(QLatin1Char(' '));
            continue;
        }

        // ---- 5) CJK 标点归一为半角 ----
        //   目的是让规则文件只需写一套标点。
        //   注意：这些替换只影响「命令解析用」的文本，
        //         历史记录保存时使用原始文本，不做修改。
        switch (u) {
        case 0x3002:   // 。
            out.append(QLatin1Char('.'));
            continue;
        case 0xFF0C:   // ，
        case 0x3001:   // 、
            out.append(QLatin1Char(','));
            continue;
        case 0xFF1F:   // ？
            out.append(QLatin1Char('?'));
            continue;
        case 0xFF01:   // ！（已被全角范围覆盖，这里保留以防万一）
            out.append(QLatin1Char('!'));
            continue;
        case 0xFF1A:   // ：
            out.append(QLatin1Char(':'));
            continue;
        case 0xFF1B:   // ；
            out.append(QLatin1Char(';'));
            continue;
        case 0x201C:   // “
        case 0x201D:   // ”
        case 0x300C:   // 「
        case 0x300D:   // 」
        case 0x300E:   // 『
        case 0x300F:   // 』
            out.append(QLatin1Char('"'));
            continue;
        default:
            break;
        }

        out.append(c);
    }

    // 折叠连续空白
    out = out.simplified();

    return out;
}

bool CommandEngine::isInputAcceptable(const QString &text, int maxLength,
                                     QString *reason)
{
    auto fail = [reason](const QString &r) {
        if (reason) *reason = r;
        return false;
    };

    // 用**规范化之后**的结果判断「是否为空」。
    //  这里踩过一个坑：原来只判 text.trimmed().isEmpty()，但 trimmed() 不处理
    //  U+200B 这类零宽字符，于是「只包含零宽字符」的输入会被判为「非空」而放行，
    //  到了匹配阶段再被 normalizeInput 清成空串 —— 结果是走完整个解析流程后
    //  报「无法识别」，而不是在入口处明确报「输入为空」。
    //
    //  对用户来说这是「我明明打了字（看不见的字）却说没输入」的困惑；
    //  从安全角度，入口校验应当以「真正参与匹配的文本」为准。
    const QString normalized = normalizeInput(text);
    if (normalized.isEmpty())
        return fail(erc::describe(erc::CommandInvalidInput));

    if (text.length() > maxLength) {
        return fail(QStringLiteral("输入过长（%1 个字符，上限 %2）。")
                        .arg(text.length()).arg(maxLength));
    }

    // 检查原始输入中的危险控制字符（规范化会剔除，但这里需要发现并拒绝）
    // 允许制表/换行/回车：用户可能从别处粘贴多行文本，
    // 规范化时会把它们折叠成空格（见 normalizeInput 第 3 步）。
    //
    // 这里必须与 normalizeInput 第 2 步的覆盖面**保持一致**：
    // C0（0x00-0x1F）、DEL（0x7F）、C1（0x80-0x9F）。
    // 曾经只统计 < 0x20，导致 0x7F 被静默删除而不是明确拒绝 ——
    // 用户看不到任何提示，只觉得"这个字符好像消失了"，无从排查。
    int dangerous = 0;
    for (const QChar &c : text) {
        const ushort u = c.unicode();
        if ((u < 0x20 && u != 0x09 && u != 0x0A && u != 0x0D)
            || u == 0x7F
            || (u >= 0x80 && u <= 0x9F)) {
            ++dangerous;
        }
        if (u == QChar::Null)
            return fail(QStringLiteral("输入中包含空字节，已拒绝。"));
    }

    if (dangerous > 0) {
        return fail(QStringLiteral("输入中包含 %1 个不可见的控制字符，已拒绝。")
                        .arg(dangerous));
    }

    return true;
}

// ===========================================================================
//  语言相容性
// ===========================================================================
bool CommandEngine::languageMatches(Language ruleLanguage, Language inputLanguage)
{
    // 通用规则（Auto）匹配任何语言
    if (ruleLanguage == Language::Auto)
        return true;

    // 输入语言无法判定时，放宽为「全部参与匹配」，
    // 因为此时规则的语言差异不应成为拒绝匹配的理由。
    if (inputLanguage == Language::Auto)
        return true;

    return ruleLanguage == inputLanguage;
}

// ===========================================================================
//  主解析入口
// ===========================================================================
ParseResult CommandEngine::parse(const QString &text, Language language) const
{
    ParseResult result;

    // ---- 0) 前置：仓库必须可用 ----
    if (!m_repository) {
        result.outcome = ParseOutcome::Invalid;
        result.errorMessage = QStringLiteral("命令规则仓库未初始化。");
        return result;
    }

    // ---- 1) 输入合法性检查（在规范化之前，以便发现控制字符）----
    QString rejectReason;
    if (!isInputAcceptable(text, m_config.maxInputLength, &rejectReason)) {
        result.outcome = ParseOutcome::Invalid;
        result.errorMessage = rejectReason;
        DC_DEBUG(LogCat::Command,
                 QStringLiteral("输入被拒绝：%1").arg(rejectReason));
        return result;
    }

    // ---- 2) 规范化 ----
    const QString normalized = normalizeInput(text);
    result.normalizedInput = normalized;

    if (normalized.isEmpty()) {
        result.outcome = ParseOutcome::Invalid;
        result.errorMessage = QStringLiteral("规范化后输入为空（可能只包含不可见字符）。");
        return result;
    }

    // ---- 3) 语言判定 ----
    Language effectiveLanguage = language;
    if (language == Language::Auto) {
        const LanguageGuess guess = LanguageDetector::detect(normalized, Language::ZhCN);
        effectiveLanguage = guess.language;
        result.detectedLanguage = guess.language;

        DC_DEBUG(LogCat::Command,
                 QStringLiteral("语言判定：%1（置信度 %2）-%3")
                     .arg(languageToCode(guess.language))
                     .arg(guess.confidence, 0, 'f', 2)
                     .arg(guess.reason));
    } else {
        result.detectedLanguage = language;
    }

    // ---- 4) 精确模式匹配 ----
    QVector<CommandCandidate> candidates = matchByPattern(normalized, effectiveLanguage);

    // ---- 5) 关键词兜底（仅当精确匹配无结果时）----
    if (candidates.isEmpty() && m_config.enableKeywordFallback) {
        candidates = matchByKeyword(normalized, effectiveLanguage);
    }

    // ---- 6) 无匹配 ----
    if (candidates.isEmpty()) {
        result.outcome = ParseOutcome::NoMatch;
        result.errorMessage = erc::describe(erc::CommandNoMatch);
        emit const_cast<CommandEngine *>(this)->parsed(result);
        return result;
    }

    // ---- 7) 排序：优先级降序 -> 置信度降序 ----
    std::sort(candidates.begin(), candidates.end(),
              [](const CommandCandidate &a, const CommandCandidate &b) {
                  if (a.priority != b.priority)
                      return a.priority > b.priority;
                  return a.confidence > b.confidence;
              });

    // ---- 8) 歧义判定 ----
    //  歧义条件：存在多条规则，且 top1 与 top2 的「优先级 + 置信度」等同。
    //
    //  SRS 4.6：对低置信度或多义输入请求澄清。
    //  注意这里只把「真正无法区分」的情况判为歧义；
    //  如果优先级不同，高优先级直接胜出，不算歧义。
    const CommandCandidate &top = candidates.first();

    QVector<CommandCandidate> ambiguous;
    for (const CommandCandidate &c : candidates) {
        const bool samePriority = c.priority == top.priority;
        const bool sameConfidence =
            qAbs(c.confidence - top.confidence) < 1e-6;

        if (samePriority && sameConfidence)
            ambiguous.append(c);
        else
            break;   // 排序已保证后续只会更差
    }

    // ---- 9) 结果填充 ----
    if (ambiguous.size() > 1) {
        // 多义：全部列出，要求用户选择
        result.outcome = ParseOutcome::Ambiguous;
        result.candidates = ambiguous;
        result.best = top;
        result.clarificationPrompt =
            buildClarificationPrompt(ambiguous, effectiveLanguage);
        result.errorMessage = erc::describe(erc::CommandAmbiguous);

        DC_DEBUG(LogCat::Command,
                 QStringLiteral("输入产生歧义：%1 个候选（%2）")
                     .arg(ambiguous.size())
                     .arg(normalized));
    } else if (top.confidence < m_config.minConfidence) {
        // 置信度不足：展示给用户确认（SRS 4.5 SPK-003 对语音同理）
        result.outcome = ParseOutcome::LowConfidence;
        result.best = top;
        result.candidates = candidates;
        result.clarificationPrompt =
            QStringLiteral("你是想执行「%1」吗？请确认后再执行。")
                .arg(top.description.isEmpty() ? top.ruleId : top.description);
        result.errorMessage = erc::describe(erc::CommandLowConfidence);
    } else {
        result.outcome = ParseOutcome::Matched;
        result.best = top;
    }

    emit const_cast<CommandEngine *>(this)->parsed(result);
    return result;
}

// ===========================================================================
//  精确模式匹配
// ===========================================================================
QVector<CommandCandidate> CommandEngine::matchByPattern(const QString &normalized,
                                                       Language language) const
{
    QVector<CommandCandidate> candidates;

    static const QRegularExpression slotRe(
        QStringLiteral("\\{([A-Za-z_][A-Za-z0-9_]*)\\}"));
    Q_UNUSED(slotRe)

    const QVector<CommandRule> rules = m_repository->enabledRules();

    for (const CommandRule &rule : rules) {
        // 语言过滤
        if (!languageMatches(rule.language, language))
            continue;

        // 禁止级规则不参与匹配（SRS 4.4：不提供）
        if (rule.riskLevel == RiskLevel::Forbidden)
            continue;

        // 规则未经编译（例如刚被加入尚未 compile）时跳过，
        // 由 CommandRepository::addRule 负责编译。
        if (rule.compiledPatterns.size() != rule.patterns.size())
            continue;

        for (int i = 0; i < rule.compiledPatterns.size(); ++i) {
            const QRegularExpression &re = rule.compiledPatterns.at(i);
            const QRegularExpressionMatch match = re.match(normalized);
            if (!match.hasMatch())
                continue;

            // 参数提取与校验
            QMap<QString, QString> params;
            QString failureReason;
            if (!extractParameters(rule, i, match, &params, &failureReason)) {
                // 参数不合法：该模式不算命中，但记录一条调试日志，
                // 便于用户排查「为什么我的输入匹配不上」
                DC_DEBUG(LogCat::Command,
                         QStringLiteral("规则 %1 模式「%2」匹配成功但参数校验失败：%3")
                             .arg(rule.id, rule.patterns.value(i), failureReason));
                continue;
            }

            CommandCandidate candidate;
            candidate.ruleId = rule.id;
            candidate.intent = rule.intent;
            candidate.matchedPattern = rule.patterns.value(i);
            candidate.parameters = params;
            candidate.confidence = m_config.exactMatchConfidence;
            candidate.riskLevel = rule.riskLevel;
            candidate.priority = rule.priority;
            candidate.description = rule.description.isEmpty()
                ? rule.patterns.value(i)
                : rule.description;

            candidates.append(candidate);

            // 同一规则的其他模式不再重复计入（避免同一规则占据多个候选位）
            break;
        }
    }

    return candidates;
}

// ===========================================================================
//  参数提取
//
//  三类参数来源：
//    Constant  -> 直接取模板固定值
//    Slot      -> 从正则捕获组取值（按声明顺序对应）
//    AliasMap  -> 用别名表把用户输入归一化（如「浏览器」-> "browser"）
//
//  每个参数取值后都要通过 validation 正则（若声明了），
//  任何一个必需参数失败则整条规则不匹配 —— 这是「执行前参数校验」的落点
//  （SRS 4.6 最后一条要求）。
// ===========================================================================
bool CommandEngine::extractParameters(const CommandRule &rule, int patternIndex,
                                     const QRegularExpressionMatch &match,
                                     QMap<QString, QString> *out,
                                     QString *failureReason) const
{
    Q_ASSERT(out);

    // 收集本模式的槽顺序
    const QString orderText = rule.patternSlotOrders.value(patternIndex);
    const QStringList slotOrder = orderText.isEmpty()
        ? QStringList()
        : orderText.split(QLatin1Char(','), Qt::SkipEmptyParts);

    // 槽名 -> 捕获值
    QHash<QString, QString> slotValues;
    for (int i = 0; i < slotOrder.size(); ++i) {
        // 捕获组 1 对应第一个槽
        const QString value = match.captured(i + 1);
        if (!value.isNull())
            slotValues.insert(slotOrder.at(i), value);
    }

    for (const ParameterTemplate &param : rule.parameters) {
        QString value;

        switch (param.source) {
        case ParameterSource::Constant:
            value = param.value;
            break;

        case ParameterSource::Slot:
            if (!slotValues.contains(param.slotName)) {
                if (param.required) {
                    if (failureReason) {
                        *failureReason = QStringLiteral("缺少必需参数「%1」。")
                                             .arg(param.name);
                    }
                    return false;
                }
                continue;
            }
            value = slotValues.value(param.slotName);
            break;

        case ParameterSource::AliasMap:
            // AliasMap 的输入来源，按优先级依次尝试：
            //   1. 以参数名本身命名的槽（最直观的写法）
            //   2. 参数显式声明的 slotName
            //   3. **本规则唯一/第一个槽**（兜底，见下）
            //
            //  第 3 条是必需的，这里踩过一个坑：
            //    规则 open_folder 声明了两个参数：
            //      folder_raw : {source:"slot",     slot:"folder", required:true}
            //      target     : {source:"alias_map", table:"folder"}
            //    模式是「打开{folder}」。槽名是 folder，被 folder_raw 消费；
            //    而 target 既没有同名槽、也没有 slotName，于是前两步都查不到，
            //    直接 return false —— 整个候选被丢弃。
            //
            //    后果不是「报错」而是「静默降级」：这条规则永远匹配不上，
            //    解析会滑到优先级更低的 open_path_absolute（「打开{path}」），
            //    用户的「打开下载文件夹」被当成一个**路径**来处理，
            //    别名归一化（下载 -> downloads）完全没有发生。
            //    现象是「打开下载文件夹没反应 / 行为怪异」，且没有任何日志。
            //
            //  为什么用「本规则第一个槽」而不是「所有槽拼起来」：
            //    这类规则的模式里通常只有一个槽，别名归一化针对的就是它。
            //    取第一个槽语义明确、不会因为多槽规则而错误拼接。
            {
                QString raw;
                if (slotValues.contains(param.name)) {
                    raw = slotValues.value(param.name);
                } else if (!param.slotName.isEmpty()
                           && slotValues.contains(param.slotName)) {
                    raw = slotValues.value(param.slotName);
                } else if (!slotOrder.isEmpty()) {
                    // 兜底：用本规则模式的第一个槽
                    raw = slotValues.value(slotOrder.first());
                }

                if (raw.isEmpty()) {
                    if (param.required) {
                        if (failureReason) {
                            *failureReason = QStringLiteral(
                                "缺少用于别名归一化的参数「%1」。").arg(param.name);
                        }
                        return false;
                    }
                    continue;
                }

                // 归一化：未命中别名表时保留原值，
                // 这样用户直接写标准值（如 app_id "browser"）也能工作。
                value = m_repository->normalizeAlias(param.value, raw);
            }
            break;
        }

        // ---- 值校验（SRS 7.3：所有外部输入均需校验）----
        if (!param.validationPattern.isEmpty()) {
            const QRegularExpression validator(param.validationPattern);
            if (validator.isValid() && !validator.match(value).hasMatch()) {
                if (failureReason) {
                    *failureReason = QStringLiteral(
                        "参数「%1」的值不符合要求（校验规则：%2）。")
                        .arg(param.name, param.validationPattern);
                }
                return false;
            }
        }

        // 值长度二次防护：防止正则校验缺失时超长输入进入执行层
        if (value.length() > 1024) {
            if (failureReason) {
                *failureReason = QStringLiteral("参数「%1」的值过长。").arg(param.name);
            }
            return false;
        }

        // 值中不得含 NUL（防截断绕过）
        if (value.contains(QChar(QChar::Null))) {
            if (failureReason) {
                *failureReason = QStringLiteral("参数「%1」的值包含非法字符。")
                                     .arg(param.name);
            }
            return false;
        }

        out->insert(param.name, value);
    }

    return true;
}

// ===========================================================================
//  关键词兜底匹配
//
//  设计意图：
//    用户说「帮我开一下浏览器」不在任何精确模式里，但包含「浏览器」
//    这个强关键词。兜底匹配允许这种情况仍能得到低置信度候选，
//    再由上层要求用户确认 —— 比直接回答「没听懂」体验更好，
//    也符合 SRS 4.6「对低置信度输入请求澄清」的要求。
//
//  评分方式（可解释、无模型）：
//    对每条规则的意图词与模式串做「关键词切分」，统计输入中命中的关键词数，
//    置信度 = 基础分 + 命中数 * 单次加分，上限 0.95。
// ===========================================================================
QVector<CommandCandidate> CommandEngine::matchByKeyword(const QString &normalized,
                                                       Language language) const
{
    QVector<CommandCandidate> candidates;

    // 关键词来源：从模式串中提取「长度 >= 2 的连续非占位符片段」
    // 例如 "打开{app}" 的关键词是 "打开"
    static const QRegularExpression slotRe(
        QStringLiteral("\\{[A-Za-z_][A-Za-z0-9_]*\\}"));

    const QString loweredInput = normalized.toLower();

    for (const CommandRule &rule : m_repository->enabledRules()) {
        if (!languageMatches(rule.language, language))
            continue;
        if (rule.riskLevel == RiskLevel::Forbidden)
            continue;

        // 高风险规则不参与关键词兜底匹配。
        // 理由：SRS 4.2 TXT-004 明确「不得猜测执行高风险操作」，
        //       模糊匹配到高风险意图本身就是危险行为。
        if (rule.riskLevel == RiskLevel::High)
            continue;

        QSet<QString> keywords;
        for (const QString &pattern : rule.patterns) {
            // 去掉占位符，按标点/空白切分
            QString stripped = pattern;
            stripped.replace(slotRe, QStringLiteral(" "));

            static const QRegularExpression separators(
                QStringLiteral("[\\s,.:;!?()\\[\\]{}\"'`/\\\\|<>+*=&%$#@~^-]+"));

            const QStringList tokens =
                stripped.split(separators, Qt::SkipEmptyParts);
            for (const QString &token : tokens) {
                // 太短的词（如单个「的」「了」）区分度低，跳过
                if (token.length() >= 2)
                    keywords.insert(token.toLower());
            }
        }

        if (keywords.isEmpty())
            continue;

        int hits = 0;
        int matchedLength = 0;
        for (const QString &kw : keywords) {
            if (loweredInput.contains(kw)) {
                ++hits;
                matchedLength += kw.length();
            }
        }

        if (hits == 0)
            continue;

        // 置信度计算
        double confidence = m_config.keywordBaseConfidence
                            + hits * m_config.keywordHitBonus;

        // 命中字符占输入的比例越高，越可信
        const double coverage = static_cast<double>(matchedLength)
                                / qMax(1, normalized.length());
        confidence += coverage * 0.2;

        // 上限 0.95：模糊匹配永远不应被当作完全可信
        confidence = qMin(0.95, confidence);

        CommandCandidate candidate;
        candidate.ruleId = rule.id;
        candidate.intent = rule.intent;

        // 模糊匹配无法可靠提取参数，因此填入规则中的常量参数，
        // 槽参数一律留空，交由执行层判断并提示用户补充。
        for (const ParameterTemplate &p : rule.parameters) {
            if (p.source == ParameterSource::Constant)
                candidate.parameters.insert(p.name, p.value);
        }

        candidate.matchedPattern = QStringLiteral("(关键词匹配)");
        candidate.confidence = confidence;
        candidate.riskLevel = rule.riskLevel;
        candidate.priority = rule.priority;   // 保留优先级但会因低置信度被要求确认
        candidate.description = rule.description.isEmpty()
            ? rule.patterns.value(0)
            : rule.description;

        candidates.append(candidate);
    }

    return candidates;
}

// ===========================================================================
//  澄清问句生成
// ===========================================================================
QString CommandEngine::buildClarificationPrompt(
    const QVector<CommandCandidate> &candidates, Language language)
{
    if (candidates.isEmpty())
        return QString();

    QStringList options;
    for (int i = 0; i < candidates.size(); ++i) {
        const CommandCandidate &c = candidates.at(i);
        const QString label = c.description.isEmpty() ? c.ruleId : c.description;
        options.append(QStringLiteral("%1. %2").arg(i + 1).arg(label));
    }

    // 按界面语言给出问句（SRS 4.2 TXT-002）
    switch (language) {
    case Language::EnUS:
        return QStringLiteral("I found multiple matching commands. Which one did you mean?\n%1")
            .arg(options.join(QLatin1Char('\n')));
    case Language::JaJP:
        return QStringLiteral("複数のコマンドが一致しました。どれを実行しますか？\n%1")
            .arg(options.join(QLatin1Char('\n')));
    case Language::ZhCN:
    case Language::Auto:
        break;
    }

    return QStringLiteral("这句话可以理解为多个命令，请选择你要执行的操作：\n%1")
        .arg(options.join(QLatin1Char('\n')));
}

} // namespace dc
