// ============================================================================
//  PathGuard.h — 路径规范化、穿越防护与敏感位置识别
//
//  需求依据：SRS V1.1
//    4.3 文件与目录：
//        「对路径进行规范化处理，阻止路径穿越和不受允许的路径访问。」
//        「默认不删除、覆盖、移动或修改文件。」
//    4.4 权限（敏感位置更严格确认）
//    7.3 安全性：「防止路径穿越、未经授权的文件覆盖和危险批处理。」
//    9.2 安全测试：「路径穿越与特殊字符输入」「尝试访问未授权目录」
//
//  设计说明（为什么这些检查放在独立模块）：
//    路径校验是最容易出错、也最容易被绕过的安全点。把它独立出来，
//    配上覆盖完整边界的单元测试（tests/unit/tst_pathguard.cpp），
//    比把 checkPath() 散落在各个调用点要可靠得多。
//
//  防护覆盖的攻击面：
//    1. ".." 上级跳转（含编码形式 %2e%2e、混合分隔符 ..\ ../）
//    2. 绝对路径逃逸（白名单是 /a/b，输入 /a/bx 不应通过）
//    3. 符号链接指向白名单外（含 Windows 目录联接 junction）
//    4. 路径长度与非法字符（Windows 保留名 CON/PRN/AUX/NUL/COM1..9/LPT1..9）
//    5. 尾部点与空格（Windows 会静默截断，导致校验结果与实际不符）
//    6. 空字节注入、控制字符
//    7. UNC 路径与设备路径（\\?\、\\.\）
// ============================================================================
#pragma once

#include "model/PermissionTypes.h"
#include "model/Types.h"

#include <QString>
#include <QStringList>

namespace dc {

// ---------------------------------------------------------------------------
// 路径校验结论
// ---------------------------------------------------------------------------
struct PathCheckResult {
    bool ok = false;
    QString errorCode;             // 见 core/ErrorCode.h（PathXxx 系列）
    QString message;               // 面向用户的说明
    QString normalizedPath;        // 规范化后的绝对路径（ok 为 true 时有效）
    bool outsideWhitelist = false; // 是否因不在白名单被拒
    bool traversalDetected = false;
    SensitivityLevel sensitivity = SensitivityLevel::Normal;
};

// ---------------------------------------------------------------------------
// 路径校验策略
// ---------------------------------------------------------------------------
struct PathPolicy {
    // 允许访问的根目录（绝对路径、已规范化）。空列表表示「不限制根目录但
    // 仍需通过其他检查」——生产环境不应使用空列表，默认配置会填入用户目录。
    QStringList allowedRoots;

    // 是否要求路径必须落在 allowedRoots 之内（默认 true，SRS 4.3）
    bool requireWhitelist = true;

    // 是否允许访问敏感位置（默认允许，但会置 sensitivity 字段，由权限模块
    // 决定是否升级确认强度。SRS 4.4 第 6 条：敏感位置实行更严格确认）
    bool allowSensitiveLocations = true;

    // 访问的目标必须是已存在（打开文件）还是允许不存在（保存/新建）
    bool requireExists = true;

    // 路径长度上限
    int maxPathLength = 4096;
};

// ---------------------------------------------------------------------------
// PathGuard
// ---------------------------------------------------------------------------
class PathGuard
{
public:
    // ---- 规范化 ----------------------------------------------------------
    // 统一处理：
    //   - 正斜杠/反斜杠统一
    //   - "." 与多余分隔符折叠
    //   - 不解析 ".."，而是保留以便 detectTraversal 判定（关键设计点）
    //   - 去掉尾部的点与空格（Windows 语义）
    //   - 展开 "~"（仅 POSIX）
    static QString normalize(const QString &path);

    // 转为绝对路径（相对于 baseDir，默认当前工作目录）
    static QString toAbsolute(const QString &path, const QString &baseDir = QString());

    // ---- 穿越检测 --------------------------------------------------------
    // 必须在 normalize 之后、resolved 之前调用，否则 ".." 已被折叠而无法检测。
    // 注意：本函数对「规范化后仍在 allowedRoots 之内」的 ".." 也会返回 true，
    //       这是刻意的保守策略 —— 用户输入中带 ".." 一律拒绝（SRS 4.3）。
    static bool detectTraversal(const QString &rawPath);

    // ---- 危险字符 --------------------------------------------------------
    static bool hasIllegalCharacters(const QString &path);
    static bool hasWindowsReservedName(const QString &path);
    static bool hasNullByte(const QString &path);
    static bool isUncOrDevicePath(const QString &path);

    // ---- 敏感位置识别（SRS 4.4 第 6 条）----------------------------------
    static SensitivityLevel classifySensitivity(const QString &absolutePath);
    static bool isSystemLocation(const QString &absolutePath);
    static bool isStartupLocation(const QString &absolutePath);

    // ---- 主校验入口 ------------------------------------------------------
    // 这是所有文件/目录访问在进入 SystemService 前必须调用的一次校验。
    static PathCheckResult check(const QString &rawPath, const PathPolicy &policy);

    // ---- 目标指纹（用于权限拒绝记录与审计）-------------------------------
    // 不直接存路径明文，而是存规范化路径的 SHA-256 前 16 字节十六进制串。
    // 这样既能在「同一目标」判定上准确，又不会在记录里泄露完整路径（SRS 7.3）。
    static QString fingerprint(const QString &absolutePath);

    // ---- 允许根目录推导 --------------------------------------------------
    // 依据当前平台给出合理的默认允许根（用户目录、桌面、文档、下载）。
    // 默认配置使用它，而不是允许整个文件系统。
    static QStringList defaultAllowedRoots();

    // ---- 路径脱敏（日志用）-----------------------------------------------
    // 只保留根与最后一级，中间用 ... 代替，例如：
    //   C:\Users\ab\Documents\report.docx  ->  C:\...\report.docx
    static QString redact(const QString &absolutePath);
};

} // namespace dc
