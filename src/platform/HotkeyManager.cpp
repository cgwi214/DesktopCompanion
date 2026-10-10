// ============================================================================
//  HotkeyManager.cpp — 全局快捷键实现
//
//  需求依据：SRS V1.1 4.12、6.2、7.3
// ============================================================================
#include "platform/HotkeyManager.h"

#include "core/ErrorCode.h"
#include "core/Logger.h"
#include "platform/PlatformAdapter.h"

#include <QGuiApplication>
#include <QSet>

#ifdef Q_OS_WIN
#  include <windows.h>
#endif

namespace dc {

// ===========================================================================
//  构造 / 析构
// ===========================================================================
HotkeyManager::HotkeyManager(PlatformAdapter *adapter, QObject *parent)
    : QObject(parent)
    , m_adapter(adapter)
{
    m_supported = isSupported(&m_unsupportedReason);

    if (!m_supported) {
        // SRS 6.2：不支持的功能应禁用并解释原因
        DC_INFO(LogCat::Platform,
                QStringLiteral("全局快捷键不可用：%1").arg(m_unsupportedReason));
    }
}

HotkeyManager::~HotkeyManager()
{
    unregisterAll();
}

// ===========================================================================
//  平台支持判定
// ===========================================================================
bool HotkeyManager::isSupported(QString *reason)
{
#ifdef Q_OS_WIN
    Q_UNUSED(reason)
    return true;
#elif defined(Q_OS_LINUX)
    // Wayland 下 X11 的 XGrabKey 不生效，全局热键需由桌面环境提供
    if (!qEnvironmentVariable("WAYLAND_DISPLAY").isEmpty()) {
        if (reason) {
            *reason = QStringLiteral(
                "当前桌面环境为 Wayland，程序无法注册全局快捷键。"
                "请在系统设置的快捷键中绑定命令："
                "desktop-companion --toggle / --history / --input");
        }
        return false;
    }
    if (reason)
        reason->clear();
    return true;
#else
    if (reason)
        *reason = QStringLiteral("当前系统不支持全局快捷键。");
    return false;
#endif
}

// ===========================================================================
//  按键序列解析
// ===========================================================================
QKeySequence HotkeyManager::parseSequence(const QString &text)
{
    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty())
        return QKeySequence();

    // "none" / "无" 表示用户主动禁用该快捷键
    if (trimmed.compare(QStringLiteral("none"), Qt::CaseInsensitive) == 0
        || trimmed == QStringLiteral("无")) {
        return QKeySequence();
    }

    // QKeySequence::fromString 已支持 "Ctrl+Alt+M" 形式，
    // 但对手写配置里的小写和多余空格不够宽容，这里先规范化。
    QStringList parts = trimmed.split(QLatin1Char('+'), Qt::SkipEmptyParts);
    if (parts.isEmpty())
        return QKeySequence();

    // 别名归一化：让 "control"/"ctrl"、"option"/"alt"、"win"/"meta" 都能识别
    static const QHash<QString, QString> aliasMap{
        { QStringLiteral("control"), QStringLiteral("Ctrl") },
        { QStringLiteral("ctl"),     QStringLiteral("Ctrl") },
        { QStringLiteral("ctrl"),    QStringLiteral("Ctrl") },
        { QStringLiteral("option"),  QStringLiteral("Alt") },
        { QStringLiteral("alt"),     QStringLiteral("Alt") },
        { QStringLiteral("shift"),   QStringLiteral("Shift") },
        { QStringLiteral("meta"),    QStringLiteral("Meta") },
        { QStringLiteral("super"),   QStringLiteral("Meta") },
        { QStringLiteral("win"),     QStringLiteral("Meta") },
        { QStringLiteral("cmd"),     QStringLiteral("Meta") },
    };

    QStringList normalized;
    for (const QString &part : parts) {
        const QString key = part.trimmed();
        if (key.isEmpty())
            continue;

        const QString lower = key.toLower();
        if (aliasMap.contains(lower)) {
            normalized.append(aliasMap.value(lower));
        } else {
            // 普通按键：首字母大写（Space / F1 / A / Esc）
            QString formatted = key;
            if (formatted.length() == 1)
                formatted = formatted.toUpper();
            else
                formatted = formatted.at(0).toUpper() + formatted.mid(1);
            normalized.append(formatted);
        }
    }

    if (normalized.isEmpty())
        return QKeySequence();

    const QKeySequence seq = QKeySequence::fromString(
        normalized.join(QLatin1Char('+')), QKeySequence::PortableText);

    return seq;
}

QString HotkeyManager::canonicalText(const QKeySequence &sequence)
{
    if (sequence.isEmpty())
        return QStringLiteral("none");
    return sequence.toString(QKeySequence::PortableText);
}

// ===========================================================================
//  冲突检测
// ===========================================================================
QVector<QPair<HotkeyAction, HotkeyAction>>
HotkeyManager::detectConflicts(const QHash<HotkeyAction, QString> &configured)
{
    QVector<QPair<HotkeyAction, HotkeyAction>> conflicts;

    // 建立「规范化文本 -> 动作列表」的倒排表
    QHash<QString, QVector<HotkeyAction>> bySequence;

    for (auto it = configured.constBegin(); it != configured.constEnd(); ++it) {
        const QKeySequence seq = parseSequence(it.value());
        if (seq.isEmpty())
            continue;   // 未设置或已禁用，不参与冲突检测

        const QString canonical = canonicalText(seq);
        bySequence[canonical].append(it.key());
    }

    for (auto it = bySequence.constBegin(); it != bySequence.constEnd(); ++it) {
        const QVector<HotkeyAction> &actions = it.value();
        if (actions.size() < 2)
            continue;

        // 两两配对输出，便于界面逐条展示
        for (int i = 0; i < actions.size(); ++i) {
            for (int j = i + 1; j < actions.size(); ++j)
                conflicts.append({ actions.at(i), actions.at(j) });
        }
    }

    return conflicts;
}

// ===========================================================================
//  注册
// ===========================================================================
HotkeyRegistrationReport HotkeyManager::registerAll(
    const QHash<HotkeyAction, QString> &configured)
{
    unregisterAll();

    HotkeyRegistrationReport report;
    report.globalHotkeySupported = m_supported;
    report.unsupportedReason = m_unsupportedReason;

    // 逐个动作处理，保持输出顺序稳定（便于测试断言）
    static const QVector<HotkeyAction> order{
        HotkeyAction::TogglePet,
        HotkeyAction::ToggleMic,
        HotkeyAction::OpenHistory,
        HotkeyAction::OpenInput,
    };

    for (HotkeyAction action : order) {
        if (!configured.contains(action))
            continue;

        HotkeyBinding binding = registerOne(action, configured.value(action));
        report.bindings.append(binding);
    }

    DC_INFO(LogCat::Platform,
            QStringLiteral("快捷键注册完成：成功 %1 个，失败 %2 个")
                .arg(report.registeredCount())
                .arg(report.failedCount()));

    emit registrationChanged(report);
    return report;
}

HotkeyBinding HotkeyManager::registerOne(HotkeyAction action, const QString &text)
{
    HotkeyBinding binding;
    binding.action = action;
    binding.rawText = text;
    binding.sequence = parseSequence(text);

    if (m_bindings.contains(action))
        unregisterOne(action);

    if (!m_supported) {
        binding.failureReason = m_unsupportedReason;
        m_bindings.insert(action, binding);
        return binding;
    }

    if (binding.sequence.isEmpty()) {
        binding.failureReason = text.trimmed().isEmpty()
            ? QStringLiteral("未设置快捷键。")
            : QStringLiteral("无法识别的按键组合「%1」。").arg(text);
        m_bindings.insert(action, binding);
        return binding;
    }

    QString reason;
    if (registerNative(action, binding.sequence, &reason)) {
        binding.registered = true;
    } else {
        // SRS 4.12：注册失败要给出可理解的原因（通常是冲突）
        binding.failureReason = reason.isEmpty()
            ? QStringLiteral("快捷键注册失败，可能是组合已被其他程序占用。")
            : reason;
        DC_WARN(LogCat::Platform,
                QStringLiteral("快捷键注册失败：%1 (%2) - %3")
                    .arg(hotkeyActionToString(action), text, binding.failureReason));
    }

    m_bindings.insert(action, binding);
    return binding;
}

void HotkeyManager::unregisterOne(HotkeyAction action)
{
    if (m_bindings.contains(action))
        unregisterNative(action);
}

void HotkeyManager::unregisterAll()
{
    const QList<HotkeyAction> actions = m_bindings.keys();
    for (HotkeyAction action : actions)
        unregisterNative(action);

    m_bindings.clear();
    m_idToAction.clear();
}

// ===========================================================================
//  原生实现
// ===========================================================================
bool HotkeyManager::registerNative(HotkeyAction action, const QKeySequence &sequence,
                                  QString *failureReason)
{
#ifdef Q_OS_WIN
    // 把 QKeySequence 的修饰键映射为 Win32 的 MOD_* 标志
    UINT modifiers = 0;
    Qt::KeyboardModifiers qtMods = sequence[0].keyboardModifiers();

    if (qtMods & Qt::ControlModifier) modifiers |= MOD_CONTROL;
    if (qtMods & Qt::AltModifier)     modifiers |= MOD_ALT;
    if (qtMods & Qt::ShiftModifier)   modifiers |= MOD_SHIFT;
    if (qtMods & Qt::MetaModifier)    modifiers |= MOD_WIN;

    // SRS 4.12：默认避免与系统保留组合冲突。Win 键组合一律拒绝，
    // 因为它们大多被系统占用，注册必然失败且可能干扰系统。
    if (modifiers & MOD_WIN) {
        if (failureReason) {
            *failureReason = QStringLiteral(
                "包含 Windows 键的组合通常被系统占用，请改用 Ctrl/Alt 组合。");
        }
        return false;
    }

    // 取主键的虚拟键码
    const int key = sequence[0].toCombined() & ~Qt::KeyboardModifierMask;
    if (key == 0) {
        if (failureReason)
            *failureReason = QStringLiteral("未能识别按键。");
        return false;
    }

    UINT vk = 0;
    if (key >= Qt::Key_A && key <= Qt::Key_Z) {
        vk = static_cast<UINT>('A' + (key - Qt::Key_A));
    } else if (key >= Qt::Key_0 && key <= Qt::Key_9) {
        vk = static_cast<UINT>('0' + (key - Qt::Key_0));
    } else if (key >= Qt::Key_F1 && key <= Qt::Key_F24) {
        vk = VK_F1 + static_cast<UINT>(key - Qt::Key_F1);
    } else {
        switch (key) {
        case Qt::Key_Space:  vk = VK_SPACE; break;
        case Qt::Key_Escape: vk = VK_ESCAPE; break;
        case Qt::Key_Tab:    vk = VK_TAB; break;
        case Qt::Key_Return:
        case Qt::Key_Enter:  vk = VK_RETURN; break;
        case Qt::Key_Home:   vk = VK_HOME; break;
        case Qt::Key_End:    vk = VK_END; break;
        case Qt::Key_PageUp: vk = VK_PRIOR; break;
        case Qt::Key_PageDown: vk = VK_NEXT; break;
        default:
            if (failureReason)
                *failureReason = QStringLiteral("该按键不支持用于全局快捷键。");
            return false;
        }
    }

    const int id = m_nextId++;

    // 全局快捷键需要 MOD_NOREPEAT（Win7+）避免长按重复触发。
    // 在 XP 上该标志不存在，动态探测以避免在 XP 上失败。
    UINT flags = 0;
    {
        // MOD_NOREPEAT 定义在 Win7 之后的头文件；老 SDK 中可能未定义。
       #ifndef MOD_NOREPEAT
       #  define MOD_NOREPEAT 0x4000
       #endif
        // 通过注册两次来探测：先带标志注册，失败则退回不带
        if (::RegisterHotKey(nullptr, id, modifiers | MOD_NOREPEAT, vk)) {
            m_idToAction.insert(id, action);
            return true;
        }
        flags = 0;
    }

    if (::RegisterHotKey(nullptr, id, modifiers | flags, vk)) {
        m_idToAction.insert(id, action);
        DC_INFO(LogCat::Platform,
                QStringLiteral("已注册全局快捷键（无 NOREPEAT）：%1")
                    .arg(canonicalText(sequence)));
        return true;
    }

    // 注册失败：取系统错误码给出精准原因
    const DWORD err = ::GetLastError();
    if (failureReason) {
        if (err == ERROR_HOTKEY_ALREADY_REGISTERED) {
            *failureReason = QStringLiteral(
                "该快捷键已被其他程序占用（或与另一项配置重复），请更换组合。");
        } else {
            *failureReason = QStringLiteral("系统拒绝注册该快捷键（错误码 %1）。").arg(err);
        }
    }
    return false;
#else
    Q_UNUSED(action)
    Q_UNUSED(sequence)
    if (failureReason) {
        *failureReason = QStringLiteral(
            "当前平台未实现全局快捷键注册。"
            "请在系统设置的快捷键中绑定命令：desktop-companion --toggle");
    }
    return false;
#endif
}

void HotkeyManager::unregisterNative(HotkeyAction action)
{
#ifdef Q_OS_WIN
    // 找到该动作对应的 id 并注销
    QList<int> ids;
    for (auto it = m_idToAction.constBegin(); it != m_idToAction.constEnd(); ++it) {
        if (it.value() == action)
            ids.append(it.key());
    }

    for (int id : ids) {
        ::UnregisterHotKey(nullptr, id);
        m_idToAction.remove(id);
    }
#else
    Q_UNUSED(action)
#endif
}

// ===========================================================================
//  查询
// ===========================================================================
HotkeyRegistrationReport HotkeyManager::currentReport() const
{
    HotkeyRegistrationReport report;
    report.globalHotkeySupported = m_supported;
    report.unsupportedReason = m_unsupportedReason;

    static const QVector<HotkeyAction> order{
        HotkeyAction::TogglePet,
        HotkeyAction::ToggleMic,
        HotkeyAction::OpenHistory,
        HotkeyAction::OpenInput,
    };

    for (HotkeyAction action : order) {
        if (m_bindings.contains(action))
            report.bindings.append(m_bindings.value(action));
    }

    return report;
}

bool HotkeyManager::isRegistered(HotkeyAction action) const
{
    return m_bindings.contains(action) && m_bindings.value(action).registered;
}

// ===========================================================================
//  原生事件入口
// ===========================================================================
bool HotkeyManager::handleNativeHotkey(int nativeId)
{
    if (!m_idToAction.contains(nativeId))
        return false;

    const HotkeyAction action = m_idToAction.value(nativeId);

    DC_DEBUG(LogCat::Platform,
             QStringLiteral("全局快捷键触发：%1").arg(hotkeyActionToString(action)));

    emit hotkeyTriggered(action);
    return true;
}

// ===========================================================================
//  Report 辅助
// ===========================================================================
int HotkeyRegistrationReport::registeredCount() const
{
    int count = 0;
    for (const HotkeyBinding &b : bindings) {
        if (b.registered)
            ++count;
    }
    return count;
}

int HotkeyRegistrationReport::failedCount() const
{
    int count = 0;
    for (const HotkeyBinding &b : bindings) {
        if (!b.registered)
            ++count;
    }
    return count;
}

QStringList HotkeyRegistrationReport::failureSummaries() const
{
    QStringList out;
    for (const HotkeyBinding &b : bindings) {
        if (b.registered)
            continue;
        out.append(QStringLiteral("%1（%2）：%3")
                       .arg(hotkeyActionDisplayName(b.action),
                            b.rawText.isEmpty() ? QStringLiteral("未设置") : b.rawText,
                            b.failureReason.isEmpty()
                                ? QStringLiteral("注册失败")
                                : b.failureReason));
    }
    return out;
}

} // namespace dc
