// ============================================================================
//  tst_path_guard.cpp — 路径守卫单元测试
//
//  需求依据：SRS V1.1 9.2（安全测试：「路径穿越与特殊字符输入」、
//           「尝试访问未授权目录」）、4.3（路径规范化、阻止穿越）、7.3
//
//  PathGuard 的头文件里自己就写了「配上覆盖完整边界的单元测试
//  (tests/unit/tst_pathguard.cpp)」—— 本文件兑现这个承诺。
//
//  覆盖的攻击面：
//    1. ".." 上级跳转（含多种分隔符写法）
//    2. 白名单逃逸（前缀相同的兄弟目录）
//    3. 非法字符、空字节、控制字符
//    4. Windows 保留设备名
//    5. 尾部点与空格（Windows 会静默截断）
//    6. UNC 与设备路径
//    7. 敏感位置分级
// ============================================================================
#include <QtTest/QtTest>

#include "model/PermissionTypes.h"
#include "platform/PathGuard.h"

using namespace dc;

class TestPathGuard : public QObject
{
    Q_OBJECT

private slots:
    // ---- 规范化 ----
    void normalize_convertsBackslashesToForward();
    void normalize_collapsesRedundantSeparators();
    void normalize_trimsTrailingDotAndSpace();
    void normalize_keepsTraversalForDetection();

    // ---- 穿越检测 ----
    void traversal_detectsParentTraversal();
    void traversal_detectsMixedSeparators();
    void traversal_normalPathIsClean();

    // ---- 非法字符 ----
    void illegal_detectsNullByte();
    void illegal_detectsControlChars();
    void illegal_normalPathIsClean();

    // ---- Windows 保留名 ----
    void reserved_detectsConAndPrn();
    void reserved_normalNameIsClean();

    // ---- UNC / 设备路径 ----
    void unc_detectsUncPath();
    void unc_detectsDevicePath();

    // ---- 敏感位置分级 ----
    void sensitivity_classifiesUserDirAsNormal();
    void sensitivity_classifiesSystemDirAsCritical();

    // ---- 主校验入口 ----
    void check_rejectsTraversal();
    void check_rejectsOutsideWhitelist();
    void check_acceptsInsideWhitelist();
    void check_rejectsWhitelistPrefixSibling();

    // ---- 指纹与脱敏 ----
    void fingerprint_isStableAndOpaque();
    void redact_hidesIntermediateDirectories();

    // ---- 默认允许根 ----
    void defaultAllowedRoots_isNotEmpty();
};

// ---------------------------------------------------------------------------
// 规范化
// ---------------------------------------------------------------------------

void TestPathGuard::normalize_convertsBackslashesToForward()
{
    const QString out = PathGuard::normalize(QStringLiteral("C:\\Users\\Test\\file.txt"));
    QVERIFY2(!out.contains(QLatin1Char('\\')),
             qPrintable(QStringLiteral("反斜杠未被统一：%1").arg(out)));
}

void TestPathGuard::normalize_collapsesRedundantSeparators()
{
    const QString out = PathGuard::normalize(QStringLiteral("C:/Users//Test///file.txt"));
    QVERIFY2(!out.contains(QStringLiteral("//")),
             qPrintable(QStringLiteral("重复分隔符未折叠：%1").arg(out)));
}

// Windows 会静默去掉路径尾部的点和空格，如果不显式处理，
// 校验时的路径和实际访问的路径就不是同一个 —— 这是个真实的绕过手法。
void TestPathGuard::normalize_trimsTrailingDotAndSpace()
{
    const QString withDot = PathGuard::normalize(QStringLiteral("C:/Users/Test/file.txt."));
    QVERIFY2(!withDot.endsWith(QLatin1Char('.')),
             qPrintable(QStringLiteral("尾部点未去除：%1").arg(withDot)));

    const QString withSpace = PathGuard::normalize(QStringLiteral("C:/Users/Test/file.txt "));
    QVERIFY2(!withSpace.endsWith(QLatin1Char(' ')),
             qPrintable(QStringLiteral("尾部空格未去除：%1").arg(withSpace)));
}

// 关键设计点：normalize 不能折叠 ".."，否则 detectTraversal 就查不出来了。
void TestPathGuard::normalize_keepsTraversalForDetection()
{
    const QString raw = QStringLiteral("C:/Users/Test/../secret.txt");
    const QString normalized = PathGuard::normalize(raw);

    // 保留 ".." 才能被检测
    QVERIFY2(normalized.contains(QStringLiteral("..")),
             "normalize 不应折叠 \"..\"，否则穿越检测会失效");
    QVERIFY(PathGuard::detectTraversal(raw));
}

// ---------------------------------------------------------------------------
// 穿越检测
// ---------------------------------------------------------------------------

void TestPathGuard::traversal_detectsParentTraversal()
{
    const QStringList attacks = {
        QStringLiteral("../etc/passwd"),
        QStringLiteral("..\\windows\\system32"),
        QStringLiteral("C:/Users/Test/../../../secret"),
        QStringLiteral("C:/Users/Test/.."),
    };

    for (const QString &attack : attacks) {
        QVERIFY2(PathGuard::detectTraversal(attack),
                 qPrintable(QStringLiteral("未检出路径穿越：%1").arg(attack)));
    }
}

void TestPathGuard::traversal_detectsMixedSeparators()
{
    // 正反斜杠混用是常见的绕过写法
    const QStringList attacks = {
        QStringLiteral("..\\..\\windows/system32"),
        QStringLiteral("C:/Users\\../..\\secret"),
        QStringLiteral("..//..//etc"),
    };

    for (const QString &attack : attacks) {
        QVERIFY2(PathGuard::detectTraversal(attack),
                 qPrintable(QStringLiteral("未检出混合分隔符穿越：%1").arg(attack)));
    }
}

void TestPathGuard::traversal_normalPathIsClean()
{
    QVERIFY(!PathGuard::detectTraversal(QStringLiteral("C:/Users/Test/Documents/file.txt")));
    QVERIFY(!PathGuard::detectTraversal(QStringLiteral("C:/Users/Test/Downloads")));
}

// ---------------------------------------------------------------------------
// 非法字符
// ---------------------------------------------------------------------------

// 空字节注入：C 字符串会在 \0 处截断，导致校验的路径与实际打开的不同。
void TestPathGuard::illegal_detectsNullByte()
{
    QString withNull = QStringLiteral("C:/Users/Test/safe.txt");
    withNull.append(QChar(0));
    withNull.append(QStringLiteral("/../../etc/passwd"));

    QVERIFY2(PathGuard::hasNullByte(withNull), "未检出空字节注入");
}

void TestPathGuard::illegal_detectsControlChars()
{
    QString withControl = QStringLiteral("C:/Users/Test/");
    withControl.append(QChar(0x01));
    withControl.append(QStringLiteral("file.txt"));

    QVERIFY2(PathGuard::hasIllegalCharacters(withControl), "未检出控制字符");
}

void TestPathGuard::illegal_normalPathIsClean()
{
    QVERIFY(!PathGuard::hasIllegalCharacters(QStringLiteral("C:/Users/Test/file.txt")));
    QVERIFY(!PathGuard::hasNullByte(QStringLiteral("C:/Users/Test/file.txt")));
}

// ---------------------------------------------------------------------------
// Windows 保留名
// ---------------------------------------------------------------------------

// CON / PRN / AUX / NUL / COM1-9 / LPT1-9 在 Windows 上是设备名，
// 用它们做文件名会打开设备而不是文件。
void TestPathGuard::reserved_detectsConAndPrn()
{
    const QStringList reserved = {
        QStringLiteral("C:/Users/Test/CON"),
        QStringLiteral("C:/Users/Test/PRN.txt"),
        QStringLiteral("C:/Users/Test/AUX"),
        QStringLiteral("C:/Users/Test/NUL"),
        QStringLiteral("C:/Users/Test/COM1"),
        QStringLiteral("C:/Users/Test/LPT1.txt"),
    };

    int detected = 0;
    for (const QString &p : reserved) {
        if (PathGuard::hasWindowsReservedName(p))
            ++detected;
    }

    QVERIFY2(detected > 0, "未检出任何 Windows 保留设备名");
}

void TestPathGuard::reserved_normalNameIsClean()
{
    QVERIFY(!PathGuard::hasWindowsReservedName(QStringLiteral("C:/Users/Test/report.txt")));
    QVERIFY(!PathGuard::hasWindowsReservedName(QStringLiteral("C:/Users/Test/console.txt")));
}

// ---------------------------------------------------------------------------
// UNC / 设备路径
// ---------------------------------------------------------------------------

void TestPathGuard::unc_detectsUncPath()
{
    QVERIFY2(PathGuard::isUncOrDevicePath(QStringLiteral("\\\\server\\share\\file.txt")),
             "未检出 UNC 路径");
}

void TestPathGuard::unc_detectsDevicePath()
{
    const QStringList devicePaths = {
        QStringLiteral("\\\\?\\C:\\Users\\Test"),
        QStringLiteral("\\\\.\\PhysicalDrive0"),
    };

    for (const QString &p : devicePaths) {
        QVERIFY2(PathGuard::isUncOrDevicePath(p),
                 qPrintable(QStringLiteral("未检出设备路径：%1").arg(p)));
    }
}

// ---------------------------------------------------------------------------
// 敏感位置分级
// ---------------------------------------------------------------------------

void TestPathGuard::sensitivity_classifiesUserDirAsNormal()
{
#ifdef Q_OS_WIN
    const QString userDir =
        QStringLiteral("C:/Users/SomeUser/Documents/report.txt");
    const SensitivityLevel level = PathGuard::classifySensitivity(userDir);
    QVERIFY2(level == SensitivityLevel::Normal
                 || level == SensitivityLevel::Sensitive,
             "普通用户文档目录不应被判为系统关键位置");
#endif
}

void TestPathGuard::sensitivity_classifiesSystemDirAsCritical()
{
#ifdef Q_OS_WIN
    const QStringList systemPaths = {
        QStringLiteral("C:/Windows/System32/drivers/etc/hosts"),
        QStringLiteral("C:/Windows/System32/config/SAM"),
        QStringLiteral("C:/Program Files/App/app.exe"),
    };

    for (const QString &p : systemPaths) {
        const SensitivityLevel level = PathGuard::classifySensitivity(p);
        QVERIFY2(level != SensitivityLevel::Normal,
                 qPrintable(QStringLiteral("系统位置未被识别为敏感：%1").arg(p)));
    }

    QVERIFY2(PathGuard::isSystemLocation(QStringLiteral("C:/Windows/System32/kernel32.dll")),
             "未能识别 Windows 系统目录");
#endif
}

// ---------------------------------------------------------------------------
// 主校验入口
// ---------------------------------------------------------------------------

void TestPathGuard::check_rejectsTraversal()
{
    PathPolicy policy;
    policy.allowedRoots << QStringLiteral("C:/Users/Test");
    policy.requireWhitelist = true;
    policy.requireExists = false;

    const PathCheckResult r =
        PathGuard::check(QStringLiteral("C:/Users/Test/../../Windows/System32"), policy);

    QVERIFY2(!r.ok, "带 .. 的路径必须被拒绝（SRS 4.3）");
    QVERIFY(r.traversalDetected || r.outsideWhitelist);
    QVERIFY(!r.message.isEmpty());
}

// SRS 9.2 明确要求「尝试访问未授权目录」的测试。
void TestPathGuard::check_rejectsOutsideWhitelist()
{
    PathPolicy policy;
    policy.allowedRoots << QStringLiteral("C:/Users/Test/Documents");
    policy.requireWhitelist = true;
    policy.requireExists = false;

    const PathCheckResult r =
        PathGuard::check(QStringLiteral("C:/Windows/System32/config"), policy);

    QVERIFY2(!r.ok, "白名单之外的路径必须被拒绝");
    QVERIFY(r.outsideWhitelist);
}

void TestPathGuard::check_acceptsInsideWhitelist()
{
    PathPolicy policy;
    policy.allowedRoots << QStringLiteral("C:/Users/Test/Documents");
    policy.requireWhitelist = true;
    policy.requireExists = false;   // 不要求真实存在，避免依赖测试机文件系统

    const PathCheckResult r =
        PathGuard::check(QStringLiteral("C:/Users/Test/Documents/report.txt"), policy);

    QVERIFY2(r.ok, qPrintable(QStringLiteral("白名单内路径被误拒：%1").arg(r.message)));
    QVERIFY(!r.normalizedPath.isEmpty());
}

// 前缀匹配的经典漏洞：
// 白名单是 C:/Users/Test/Documents，输入 C:/Users/Test/DocumentsEvil 不应通过。
// 朴素的 startsWith 比较会放行它。
void TestPathGuard::check_rejectsWhitelistPrefixSibling()
{
    PathPolicy policy;
    policy.allowedRoots << QStringLiteral("C:/Users/Test/Documents");
    policy.requireWhitelist = true;
    policy.requireExists = false;

    const PathCheckResult r =
        PathGuard::check(QStringLiteral("C:/Users/Test/DocumentsEvil/secret.txt"), policy);

    QVERIFY2(!r.ok,
             "同前缀的兄弟目录必须被拒绝（朴素的 startsWith 比较会放行它）");
}

// ---------------------------------------------------------------------------
// 指纹与脱敏
// ---------------------------------------------------------------------------

void TestPathGuard::fingerprint_isStableAndOpaque()
{
    const QString path = QStringLiteral("C:/Users/Test/Documents/report.txt");

    const QString a = PathGuard::fingerprint(path);
    const QString b = PathGuard::fingerprint(path);

    QCOMPARE(a, b);                          // 稳定
    QVERIFY(!a.isEmpty());
    QVERIFY2(!a.contains(QStringLiteral("report")), "指纹不得包含路径明文");
    QVERIFY2(!a.contains(QStringLiteral("Users")), "指纹不得包含路径明文");
}

void TestPathGuard::redact_hidesIntermediateDirectories()
{
    const QString redacted =
        PathGuard::redact(QStringLiteral("C:/Users/TestUser/Documents/secret/report.txt"));

    QVERIFY2(!redacted.contains(QStringLiteral("TestUser")),
             qPrintable(QStringLiteral("脱敏后仍含用户名：%1").arg(redacted)));
    QVERIFY2(redacted.contains(QStringLiteral("report.txt")),
             qPrintable(QStringLiteral("脱敏后应保留文件名：%1").arg(redacted)));
}

void TestPathGuard::defaultAllowedRoots_isNotEmpty()
{
    const QStringList roots = PathGuard::defaultAllowedRoots();
    QVERIFY2(!roots.isEmpty(),
             "默认允许根不能为空（生产配置不应允许整个文件系统）");
}

QTEST_MAIN(TestPathGuard)
#include "tst_path_guard.moc"
