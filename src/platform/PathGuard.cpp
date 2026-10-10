// ============================================================================
//  PathGuard.cpp — 路径安全校验实现
//
//  需求依据：SRS V1.1 4.3、4.4、7.3、9.2
//
//  实现要点说明：
//    这是整个项目安全性的关键文件，因此实现策略是「保守优先」：
//    任何无法明确判定为安全的输入，一律拒绝，并给出可解释的错误码。
// ============================================================================
#include "platform/PathGuard.h"

#include "core/BuildConfig.h"
#include "core/ErrorCode.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <QStandardPaths>

namespace dc {

// ===========================================================================
//  规范化
// ===========================================================================
QString PathGuard::normalize(const QString &path)
{
    if (path.isEmpty())
        return QString();

    QString p = path;

    // 1) 去掉 NUL 及其后的所有内容（空字节注入）
    const int nulIndex = p.indexOf(QChar(QChar::Null));
    if (nulIndex >= 0)
        p.truncate(nulIndex);

    // 2) 去掉两边空白（但注意：「尾部空格」在 Windows 上是路径的一部分语义，
    //    会被静默截断。这里去掉两端空白，同时下方会单独检测「原始输入尾部
    //    有空白但去掉后路径真实存在」这类歧义情况）。
    p = p.trimmed();

    // 3) 统一分隔符为 '/'
    p.replace(QLatin1Char('\\'), QLatin1Char('/'));

    // 4) POSIX 的 ~ 展开
   #ifndef Q_OS_WIN
    if (p == QStringLiteral("~"))
        p = QDir::homePath();
    else if (p.startsWith(QStringLiteral("~/")))
        p = QDir::homePath() + p.mid(1);
   #endif

    // 5) 折叠重复分隔符（但保留 UNC 前缀的 "//"）
    const bool unc = p.startsWith(QStringLiteral("//"));
    p.replace(QRegularExpression(QStringLiteral("/{2,}")), QStringLiteral("/"));
    if (unc && !p.startsWith(QStringLiteral("//")))
        p.prepend(QLatin1Char('/'));

    // 6) 去掉 "./"（当前目录引用）—— 它不改变路径语义，但会干扰白名单前缀比较
    //    注意：不折叠 ".."，因为穿越检测需要它
    while (p.contains(QStringLiteral("/./")))
        p.replace(QStringLiteral("/./"), QStringLiteral("/"));
    if (p.endsWith(QStringLiteral("/.")))
        p.chop(2);
    if (p.startsWith(QStringLiteral("./")))
        p.remove(0, 2);

    // 7) 去掉尾部分隔符（根目录除外）
    while (p.length() > 1 && p.endsWith(QLatin1Char('/'))) {
        // 保留 Windows 盘符根 "C:/" 与 POSIX 根 "/"
        if (p.length() == 3 && p.at(1) == QLatin1Char(':'))
            break;
        p.chop(1);
    }

    // 8) 去掉每个路径分量的尾部点与空格（Windows 语义：
    //    "C:\a \b." 实际访问的是 "C:\a\b"）
   #ifdef Q_OS_WIN
    {
        QStringList parts = p.split(QLatin1Char('/'), Qt::KeepEmptyParts);
        for (int i = 0; i < parts.size(); ++i) {
            // 跳过盘符分量与 ".."/"." 这类特殊分量
            if (i == 0 && parts.at(i).endsWith(QLatin1Char(':')))
                continue;
            if (parts.at(i) == QStringLiteral("..") || parts.at(i) == QStringLiteral("."))
                continue;
            QString seg = parts.at(i);
            while (!seg.isEmpty()
                   && (seg.endsWith(QLatin1Char('.')) || seg.endsWith(QLatin1Char(' ')))) {
                seg.chop(1);
            }
            parts[i] = seg;
        }
        p = parts.join(QLatin1Char('/'));
    }
   #endif

    return p;
}

QString PathGuard::toAbsolute(const QString &path, const QString &baseDir)
{
    const QString normalized = QString::fromLatin1(path.toUtf8());
    if (normalized.isEmpty())
        return QString();

    // Windows 盘符绝对路径（C:/...）或 POSIX 绝对路径（/...）或 UNC（//server）
    const bool isAbsolute =
        normalized.startsWith(QLatin1Char('/'))
        || (normalized.length() >= 2
            && normalized.at(1) == QLatin1Char(':')
            && normalized.at(0).isLetter());

    QString result;
    if (isAbsolute) {
        result = QDir::cleanPath(normalized);
    } else {
        const QString base = baseDir.isEmpty() ? QDir::currentPath() : baseDir;
        result = QDir::cleanPath(base + QLatin1Char('/') + normalized);
    }

    return result;
}

// ===========================================================================
//  穿越检测
// ===========================================================================
bool PathGuard::detectTraversal(const QString &rawPath)
{
    QString p = rawPath;

    // 先做 URL/百分号编码还原，攻击者常用 %2e%2e%2f 绕过朴素检查。
    // 这里做一轮覆盖最常见的编码，解码后统一检查。
    static const QVector<QPair<QString, QString>> decodings{
        { QStringLiteral("%2e"), QStringLiteral(".") },
        { QStringLiteral("%2E"), QStringLiteral(".") },
        { QStringLiteral("%2f"), QStringLiteral("/") },
        { QStringLiteral("%2F"), QStringLiteral("/") },
        { QStringLiteral("%5c"), QStringLiteral("\\") },
        { QStringLiteral("%5C"), QStringLiteral("\\") },
        { QStringLiteral("%00"), QStringLiteral("\0") },
    };
    for (const auto &d : decodings)
        p.replace(d.first, d.second);

    // 统一分隔符
    p.replace(QLatin1Char('\\'), QLatin1Char('/'));

    // 检测 ".." 作为独立路径分量出现
    const QStringList parts = p.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    for (const QString &part : parts) {
        if (part == QStringLiteral(".."))
            return true;
        // "..." 或 "..%20" 之类变体：只要以两个点开头且后续全是点/空格，也算
        if (part.startsWith(QStringLiteral(".."))) {
            bool allDotsOrSpace = true;
            for (const QChar &c : part) {
                if (c != QLatin1Char('.') && c != QLatin1Char(' ')) {
                    allDotsOrSpace = false;
                    break;
                }
            }
            if (allDotsOrSpace)
                return true;
        }
    }

    return false;
}

// ===========================================================================
//  危险字符
// ===========================================================================
bool PathGuard::hasIllegalCharacters(const QString &path)
{
    if (path.isEmpty())
        return true;

    // Windows 非法字符：< > : " | ? *（':' 只允许出现在盘符位置）
    // 跨平台一律按最严格标准检查，因为配置文件可能是从别的系统拷来的。
    static const QRegularExpression illegal(QStringLiteral("[<>\"|?*]"));

    if (path.length() >= 2 && path.at(1) == QLatin1Char(':')) {
        // 盘符形式：检查去掉盘符后的部分
        if (illegal.match(path.mid(2)).hasMatch())
            return true;
    } else if (illegal.match(path).hasMatch()) {
        return true;
    }

    // 控制字符（0x00-0x1F，其中 0x00 已被 normalize 处理）
    for (const QChar &c : path) {
        if (c.unicode() < 0x20)
            return true;
    }

    return false;
}

bool PathGuard::hasWindowsReservedName(const QString &path)
{
    // Windows 保留设备名。即使是 "CON.txt" 也会被解析为设备 CON，必须拒绝。
    static const QStringList reserved{
        QStringLiteral("CON"), QStringLiteral("PRN"), QStringLiteral("AUX"),
        QStringLiteral("NUL"),
        QStringLiteral("COM1"), QStringLiteral("COM2"), QStringLiteral("COM3"),
        QStringLiteral("COM4"), QStringLiteral("COM5"), QStringLiteral("COM6"),
        QStringLiteral("COM7"), QStringLiteral("COM8"), QStringLiteral("COM9"),
        QStringLiteral("LPT1"), QStringLiteral("LPT2"), QStringLiteral("LPT3"),
        QStringLiteral("LPT4"), QStringLiteral("LPT5"), QStringLiteral("LPT6"),
        QStringLiteral("LPT7"), QStringLiteral("LPT8"), QStringLiteral("LPT9"),
        // Windows 10+ 新增的保留名
        QStringLiteral("CONIN$"), QStringLiteral("CONOUT$"),
    };

    QString p = path;
    p.replace(QLatin1Char('\\'), QLatin1Char('/'));

    const QStringList parts = p.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    for (const QString &part : parts) {
        // 去掉扩展名后再比较：CON.txt 同样被保留
        const QString stem = part.section(QLatin1Char('.'), 0, 0);
        if (reserved.contains(stem, Qt::CaseInsensitive))
            return true;
    }

    return false;
}

bool PathGuard::hasNullByte(const QString &path)
{
    return path.contains(QChar(QChar::Null));
}

// 严重教训（必读，勿删）：
// 本函数曾经因为「行尾注释的最后一个字符是反斜杠」而整体失效：
//     // 设备命名空间：\\?\  \\.\      <-- 行尾的反斜杠是「续行符」
// C/C++ 里行尾的反斜杠会把下一行并入当前行。于是那行注释把紧随其后的
//     if (p.startsWith("\\\\?\\") || ...)
//     return true;
// 中的 if 行一起吞掉，只剩下一句孤立的
//     return true;
// 变成无条件返回 —— 结果**任何路径都被判定为 UNC/设备路径**，
// check() 一律以 DC-4003 短路返回，整个文件访问能力瘫痪；
// 同时真正的设备路径检查被静默删除，安全防护也一并消失。
// 现象是「编译无错、无告警等级、运行时毫无提示，只是什么都不工作」。
// 因此：在注释里提到 Windows 路径时，绝不要把反斜杠留在行尾。
bool PathGuard::isUncOrDevicePath(const QString &path)
{
    const QString p = path;

    // UNC 形式，两种写法都要拦：
    //   1) 正斜杠写法  //server/share
    //   2) 反斜杠写法  两个反斜杠开头（Windows 的标准 UNC 写法）
    // 只查正斜杠版是不够的 —— 用户与配置文件里最常见的恰恰是反斜杠版。
    // 这个分支此前因上面的「行尾反斜杠吞行」事故一并丢失，现补回。
    if (p.startsWith(QStringLiteral("//")))
        return true;
    if (p.startsWith(QStringLiteral("\\\\")) && !p.startsWith(QStringLiteral("\\\\?\\"))
        && !p.startsWith(QStringLiteral("\\\\.\\")))
        return true;

    // 设备命名空间（两种写法都查）：反斜杠四连问号形式，与正斜杠 //?/ 形式
    if (p.startsWith(QStringLiteral("\\\\?\\")) || p.startsWith(QStringLiteral("\\\\.\\")))
        return true;

    // 上一条的「正斜杠版」：把反斜杠统一成正斜杠后同样要拦住
    if (p.startsWith(QStringLiteral("//?/")) || p.startsWith(QStringLiteral("//./")))
        return true;

    // 卷影/设备路径（Win32 设备命名空间内部形式）
    if (p.startsWith(QStringLiteral("\\Device\\"), Qt::CaseInsensitive))
        return true;

    return false;
}

// ===========================================================================
//  敏感位置识别
// ===========================================================================
namespace {

// 把路径转为统一的正斜杠小写形式，用于前缀比较
QString canonicalForCompare(const QString &path)
{
    QString p = QDir::cleanPath(path);
    p.replace(QLatin1Char('\\'), QLatin1Char('/'));
    return p.toLower();
}

bool isUnder(const QString &path, const QString &root)
{
    if (root.isEmpty())
        return false;
    QString p = canonicalForCompare(path);
    QString r = canonicalForCompare(root);

    if (p == r)
        return true;
    if (!r.endsWith(QLatin1Char('/')))
        r += QLatin1Char('/');
    // 必须比对到「目录边界」，否则 /a/bc 会被误判为在 /a/b 之下
    return p.startsWith(r);
}

QStringList criticalLocations()
{
    QStringList locations;

   #ifdef Q_OS_WIN
    const QString sysRoot = qEnvironmentVariable("SystemRoot", QStringLiteral("C:/Windows"));
    locations << sysRoot
              << QStringLiteral("C:/Windows")
              << QStringLiteral("C:/Program Files")
              << QStringLiteral("C:/Program Files (x86)")
              << QStringLiteral("C:/ProgramData")
              << QStringLiteral("C:/$Recycle.Bin")
              << QStringLiteral("C:/Recovery")
              << QStringLiteral("C:/System Volume Information");

    // 注册表相关目录（离线 hive）
    locations << QStringLiteral("C:/Windows/System32/config");
   #else
    locations << QStringLiteral("/etc")
              << QStringLiteral("/bin")
              << QStringLiteral("/sbin")
              << QStringLiteral("/usr/bin")
              << QStringLiteral("/usr/sbin")
              << QStringLiteral("/boot")
              << QStringLiteral("/sys")
              << QStringLiteral("/proc")
              << QStringLiteral("/dev")
              << QStringLiteral("/var/lib")
              << QStringLiteral("/root");
   #endif

    return locations;
}

QStringList sensitiveLocations()
{
    QStringList locations;

   #ifdef Q_OS_WIN
    // 用户级配置与启动项位置（SRS 4.4 第 6 条明确点名「启动项」）
    const QString appData = qEnvironmentVariable("APPDATA");
    if (!appData.isEmpty()) {
        locations << appData + QStringLiteral("/Microsoft/Windows/Start Menu/Programs/Startup")
                  << appData + QStringLiteral("/Microsoft/Windows/Start Menu/Programs/Startup")
                  << appData + QStringLiteral("/Microsoft/Windows/Start Menu");
    }

    const QString programData = qEnvironmentVariable("ProgramData");
    if (!programData.isEmpty())
        locations << programData;

    // 系统级启动目录（需要管理员权限才能写，但仍要标为敏感）
    const QString sysRoot = qEnvironmentVariable("SystemRoot", QStringLiteral("C:/Windows"));
    locations << sysRoot + QStringLiteral("/System32/Tasks");

    // SSH 密钥、凭据目录
    const QString userProfile = qEnvironmentVariable("USERPROFILE");
    if (!userProfile.isEmpty()) {
        locations << userProfile + QStringLiteral("/.ssh")
                  << userProfile + QStringLiteral("/.gnupg")
                  << userProfile + QStringLiteral("/.aws")
                  << userProfile + QStringLiteral("/.azure")
                  << userProfile + QStringLiteral("/.kube")
                  << userProfile + QStringLiteral("/AppData/Local/Microsoft/Credentials");
    }
   #else
    const QString home = QDir::homePath();
    locations << home + QStringLiteral("/.config/autostart")
              << home + QStringLiteral("/.ssh")
              << home + QStringLiteral("/.gnupg")
              << home + QStringLiteral("/.aws")
              << home + QStringLiteral("/.config/systemd/user");
   #endif

    return locations;
}

} // namespace

SensitivityLevel PathGuard::classifySensitivity(const QString &absolutePath)
{
    const QString canonical = canonicalForCompare(absolutePath);

    for (const QString &loc : criticalLocations()) {
        if (isUnder(canonical, loc))
            return SensitivityLevel::Critical;
    }

    for (const QString &loc : sensitiveLocations()) {
        if (isUnder(canonical, loc))
            return SensitivityLevel::Sensitive;
    }

    return SensitivityLevel::Normal;
}

bool PathGuard::isSystemLocation(const QString &absolutePath)
{
    return classifySensitivity(absolutePath) == SensitivityLevel::Critical;
}

bool PathGuard::isStartupLocation(const QString &absolutePath)
{
    const QString canonical = canonicalForCompare(absolutePath);

   #ifdef Q_OS_WIN
    const QString appData = qEnvironmentVariable("APPDATA");
    if (!appData.isEmpty()) {
        if (isUnder(canonical, appData + QStringLiteral("/Microsoft/Windows/Start Menu/Programs/Startup")))
            return true;
    }
    // 注册表 Run 键无法用路径表达，由 AutostartManager 单独处理
   #else
    if (isUnder(canonical, QDir::homePath() + QStringLiteral("/.config/autostart")))
        return true;
   #endif

    return false;
}

// ===========================================================================
//  主校验入口
// ===========================================================================
PathCheckResult PathGuard::check(const QString &rawPath, const PathPolicy &policy)
{
    PathCheckResult result;

    // ---- 1) 基本形态检查 ----
    if (rawPath.trimmed().isEmpty()) {
        result.errorCode = erc::PathEmpty;
        result.message = erc::describe(erc::PathEmpty);
        return result;
    }

    if (hasNullByte(rawPath)) {
        result.errorCode = erc::PathInvalidCharacters;
        result.message = QStringLiteral("路径中包含空字节，已拒绝。");
        return result;
    }

    if (rawPath.length() > policy.maxPathLength) {
        result.errorCode = erc::PathInvalidCharacters;
        result.message = QStringLiteral("路径过长（%1 个字符，上限 %2），已拒绝。")
                             .arg(rawPath.length()).arg(policy.maxPathLength);
        return result;
    }

    if (isUncOrDevicePath(rawPath)) {
        // 网络共享与设备路径会绕过本地白名单语义，一律拒绝（SRS 7.3）
        result.errorCode = erc::PathNotAllowed;
        result.message = QStringLiteral("不支持网络共享路径与设备路径。");
        return result;
    }

    if (hasIllegalCharacters(rawPath)) {
        result.errorCode = erc::PathInvalidCharacters;
        result.message = erc::describe(erc::PathInvalidCharacters);
        return result;
    }

    if (hasWindowsReservedName(rawPath)) {
        result.errorCode = erc::PathInvalidCharacters;
        result.message = QStringLiteral("路径中包含系统保留名称（如 CON、NUL、COM1）。");
        return result;
    }

    // ---- 2) 穿越检测（必须在 normalize 之前，因为 normalize 不保留 ".." 语义
    //          之外的线索；这里对原始输入检查）----
    if (detectTraversal(rawPath)) {
        result.traversalDetected = true;
        result.errorCode = erc::PathTraversalDetected;
        result.message = erc::describe(erc::PathTraversalDetected);
        return result;
    }

    // ---- 3) 规范化 ----
    const QString normalized = normalize(rawPath);
    if (normalized.isEmpty()) {
        result.errorCode = erc::PathEmpty;
        result.message = erc::describe(erc::PathEmpty);
        return result;
    }

    const QString absolute = toAbsolute(normalized);
    if (absolute.isEmpty()) {
        result.errorCode = erc::PathEmpty;
        result.message = erc::describe(erc::PathEmpty);
        return result;
    }

    // ---- 4) 符号链接解析 ----
    // 关键：必须在白名单比较之前解析符号链接，否则可以通过
    // 「白名单内的软链接 -> 白名单外的目标」绕过限制。
    QFileInfo info(absolute);
    QString resolved = absolute;

    if (info.isSymLink()) {
        const QString target = info.symLinkTarget();
        if (!target.isEmpty()) {
            const QString targetNorm = normalize(target);
            resolved = toAbsolute(targetNorm, info.absolutePath());
        }
    }

    // 逐级向上检查，防止中间目录是符号链接
    {
        QString probe = resolved;
        int guard = 0;
        while (!probe.isEmpty() && guard++ < 64) {
            const QFileInfo pi(probe);
            if (pi.isSymLink()) {
                const QString target = pi.symLinkTarget();
                if (!target.isEmpty()) {
                    resolved = toAbsolute(normalize(target), pi.absolutePath());
                }
            }
            const QString parent = QFileInfo(probe).absolutePath();
            if (parent == probe)
                break;
            probe = parent;
        }
    }

    result.normalizedPath = QDir::cleanPath(resolved);

    // ---- 5) 白名单检查 ----
    if (policy.requireWhitelist) {
        bool inside = false;
        for (const QString &root : policy.allowedRoots) {
            const QString rootNorm = toAbsolute(normalize(root));
            if (rootNorm.isEmpty())
                continue;
            if (isUnder(result.normalizedPath, rootNorm)) {
                inside = true;
                break;
            }
        }

        if (!inside) {
            result.outsideWhitelist = true;
            result.errorCode = erc::PathNotAllowed;
            result.message = erc::describe(erc::PathNotAllowed);
            return result;
        }
    }

    // ---- 6) 敏感位置分级（SRS 4.4 第 6 条）----
    result.sensitivity = classifySensitivity(result.normalizedPath);

    if (result.sensitivity != SensitivityLevel::Normal && !policy.allowSensitiveLocations) {
        result.errorCode = erc::PathSensitiveLocation;
        result.message = erc::describe(erc::PathSensitiveLocation);
        return result;
    }

    // ---- 7) 存在性检查 ----
    const QFileInfo finalInfo(result.normalizedPath);
    if (policy.requireExists && !finalInfo.exists()) {
        result.errorCode = erc::PathNotFound;
        result.message = erc::describe(erc::PathNotFound);
        return result;
    }

    // 可读性检查（打开文件时）
    if (policy.requireExists && !finalInfo.isReadable()) {
        result.errorCode = erc::PathNotAccessible;
        result.message = erc::describe(erc::PathNotAccessible);
        return result;
    }

    result.ok = true;
    return result;
}

// ===========================================================================
//  目标指纹
// ===========================================================================
QString PathGuard::fingerprint(const QString &absolutePath)
{
    const QString canonical = canonicalForCompare(absolutePath);
    QByteArray data = canonical.toUtf8();

    const QByteArray digest =
        QCryptographicHash::hash(data, QCryptographicHash::Sha256);

    // 用 16 字节（32 个十六进制字符）足够避免碰撞，且不泄露原路径
    return QString::fromLatin1(digest.toHex().left(32));
}

// ===========================================================================
//  默认允许根目录
// ===========================================================================
QStringList PathGuard::defaultAllowedRoots()
{
    QStringList roots;

    // 用户目录下的常用位置（SRS 4.3：默认不访问系统目录）
    // 标准用户目录。逐个取，不构造列表 —— 这些是枚举值，
    // 需要经 writableLocation() 转换才能得到实际路径。
    static const QStandardPaths::StandardLocation kStandardLocations[] = {
        QStandardPaths::DesktopLocation,
        QStandardPaths::DocumentsLocation,
        QStandardPaths::DownloadLocation,
        QStandardPaths::PicturesLocation,
        QStandardPaths::MusicLocation,
        QStandardPaths::MoviesLocation,
    };

    for (const QStandardPaths::StandardLocation loc : kStandardLocations) {
        const QString p = QStandardPaths::writableLocation(loc);
        if (!p.isEmpty() && !roots.contains(p)) {
            roots.append(p);
        }
    }

    // 用户主目录本身（覆盖桌面/文档之外的用户文件）
    const QString home = QDir::homePath();
    if (!home.isEmpty() && !roots.contains(home))
        roots.append(home);

    return roots;
}

// ===========================================================================
//  路径脱敏
// ===========================================================================
QString PathGuard::redact(const QString &absolutePath)
{
    if (absolutePath.isEmpty())
        return QString();

    QString p = absolutePath;
    p.replace(QLatin1Char('\\'), QLatin1Char('/'));

    // Windows: C:/a/b/c.txt -> C:/.../c.txt
    // POSIX:   /a/b/c.txt   -> /.../c.txt
    const QStringList parts = p.split(QLatin1Char('/'), Qt::SkipEmptyParts);

    if (parts.size() <= 2)
        return absolutePath;

    QString root;
   #ifdef Q_OS_WIN
    if (!parts.isEmpty() && parts.first().endsWith(QLatin1Char(':')))
        root = parts.first();
   #endif
    if (root.isEmpty())
        root = QStringLiteral("/");

    const QString last = parts.last();

    if (root.endsWith(QLatin1Char(':')))
        return root + QStringLiteral("/.../") + last;
    return QStringLiteral("/.../") + last;
}

} // namespace dc
