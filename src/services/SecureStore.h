#pragma once

// ============================================================================
//  SecureStore —— 敏感配置与 API 密钥的安全存储（SRS 4.10 / 11）
//
//  需求原文（SRS 4.10 隐私与安全）：
//    "对敏感配置和 API 密钥使用操作系统提供的安全存储能力；
//     旧系统不支持时，提供风险说明和替代方案。"
//
//  设计目标：
//    1. 密钥绝不落明文磁盘。首选走操作系统凭据库。
//    2. 能力探测优先（SRS 6.2）：先问 SystemInfo 能不能用，再决定策略。
//       ——不靠"操作系统名字"判断，靠运行时探测。
//    3. 旧系统（XP / 无 DPAPI / 无 keyring）降级时必须"显式告知风险"，
//       不允许静默降级。降级结果通过 StorageMode 与 riskNotice() 暴露给 UI，
//       由 UI 强制展示一次知情确认。
//    4. 明文回退必须由用户显式开启（allowPlaintextFallback）。
//       默认策略是"拒绝保存"，宁可让翻译功能不可用，也不偷偷存密钥。
//
//  存储后端（StorageMode）：
//    - SystemKeyring : 系统凭据库（Windows DPAPI 保护的用户级文件 / GNOME Keyring
//                      经 libsecret；本实现走 DPAPI 路径，见 .cpp 说明）
//    - ProtectedFile : 受操作系统用户身份保护的文件（Windows DPAPI；
//                      Linux 无 DPAPI 时退化为 0600 权限文件 = 见 PlaintextFile）
//    - PlaintextFile : 明文文件 + 警告。仅在用户显式同意后使用。
//    - Unavailable   : 无法安全存储。所有需要密钥的功能会被禁用并给出原因。
//
//  与其它模块的关系：
//    - TranslationService 是唯一的主要消费者（翻译 API 密钥）。
//    - Logger 会对写入日志的内容做脱敏；SecureStore 额外保证
//      "密钥本身永不出现在返回值以外的任何地方"，并且
//      keyFingerprint() 是唯一允许进日志的形态。
// ============================================================================

#include <QString>
#include <QStringList>
#include <QHash>
#include <QMutex>
#include <QObject>

namespace dc {

// ---------------------------------------------------------------------------
// 存储模式
// ---------------------------------------------------------------------------
enum class StorageMode {
    Unavailable = 0,   // 无法安全存储 —— 功能被禁用
    PlaintextFile,     // 明文文件（需用户显式同意，有风险提示）
    ProtectedFile,     // 操作系统用户身份保护的文件（Windows DPAPI）
    SystemKeyring      // 系统凭据库
};

QString storageModeToString(StorageMode mode);
StorageMode storageModeFromString(const QString &text);

// 供 UI 展示：这个模式对用户意味着什么风险
QString storageModeDescription(StorageMode mode);
bool storageModeIsRisky(StorageMode mode);

// ---------------------------------------------------------------------------
// 单条密钥的读取结果
// ---------------------------------------------------------------------------
struct SecretValue {
    QString key;          // 逻辑键名，例如 "translation.api_key"
    QString value;        // 明文值 —— 仅交给使用者，永不进日志
    bool    found = false;
    StorageMode mode = StorageMode::Unavailable;

    bool isValid() const { return found && !value.isEmpty(); }
};

// ---------------------------------------------------------------------------
// 初始化结果
// ---------------------------------------------------------------------------
struct SecureStoreInitResult {
    bool ok = false;
    StorageMode mode = StorageMode::Unavailable;
    QString reason;         // 失败或降级原因（中文，可直接给用户看）
    QString riskNotice;     // 非空表示需要 UI 强制展示风险确认
    bool needsUserConsent = false;  // true 时必须先拿到用户同意才能用降级模式
    QStringList capabilities;       // 探测到的能力明细，便于诊断
};

// ---------------------------------------------------------------------------
// 设置项
// ---------------------------------------------------------------------------
struct SecureStoreConfig {
    // 存储目录（由 AppPaths 决定，通常是用户数据目录下的 secure/）
    QString storeDirectory;

    // 允许用户在知情后选择明文回退。
    // 默认 false —— 宁可功能不可用，也不静默降低安全等级。
    bool allowPlaintextFallback = false;

    // 是否已经就降级模式取得过用户同意（持久化在配置里）
    bool plaintextConsentGranted = false;

    // 允许在内存中缓存解出的密钥，减少磁盘往返。
    // 缓存只在进程生命周期内有效，不落盘。
    bool enableMemoryCache = true;
};

// ============================================================================
// SecureStore
// ============================================================================
class SecureStore : public QObject
{
    Q_OBJECT

public:
    explicit SecureStore(QObject *parent = nullptr);
    ~SecureStore() override;

    // ---- 生命周期 ----

    // 探测并初始化后端。不抛异常，失败落在 result.reason 里。
    SecureStoreInitResult initialize(const SecureStoreConfig &config);

    // 已初始化的模式
    StorageMode mode() const;

    // 是否可用（Unavailable 时所有 get/set 都会失败）
    bool isAvailable() const;

    // 是否需要用户先做风险确认
    bool needsUserConsent() const;

    // 风险说明文本（Unavailable / PlaintextFile 时非空）
    QString riskNotice() const;

    // 用户在知情后同意使用降级模式。
    // 调用后 persist consent 由调用方写进 ConfigManager。
    void grantPlaintextConsent();

    // ---- 读写 ----

    // 保存密钥。失败时返回 false 并填充 errorMessage。
    bool setSecret(const QString &key, const QString &value, QString *errorMessage = nullptr);

    // 读取密钥
    SecretValue secret(const QString &key) const;

    // 是否已存在
    bool contains(const QString &key) const;

    // 删除
    bool removeSecret(const QString &key, QString *errorMessage = nullptr);

    // 清空全部
    bool clearAll(QString *errorMessage = nullptr);

    // 列出所有键名（只列键名，不列值）
    QStringList keys() const;

    // ---- 诊断 ----

    // 唯一允许进日志的密钥标识形态：短哈希指纹
    static QString keyFingerprint(const QString &key);

    // 强制清空内存缓存（例如切换到需要隐私的场景、锁屏、程序进入后台）
    void clearMemoryCache();

    // 把当前后端状态描述成一行中文，供设置页展示
    QString describeBackend() const;

    // 后端是否支持"加密到当前用户"（DPAPI 类）
    static bool detectProtectedStorage(QString *detail = nullptr);

signals:
    // 后端模式发生变化（例如从 Unavailable 变为可用）
    void modeChanged(StorageMode mode);

    // 发生需要用户注意的安全事件（明文回退、读取失败等）
    void securityNotice(const QString &message);

private:
    // 两个后端各自的实现
    bool writeBackend(const QHash<QString, QString> &data, QString *errorMessage);
    bool readBackend(QHash<QString, QString> *data, QString *errorMessage) const;

    QString backendFilePath() const;
    bool ensureDirectory(QString *errorMessage) const;

    // PlaintextFile 模式下直接读写 JSON
    bool writePlaintext(const QHash<QString, QString> &data, QString *errorMessage);
    bool readPlaintext(QHash<QString, QString> *data, QString *errorMessage) const;

    // ProtectedFile 模式下用 DPAPI 加解密（Windows）；其它平台返回 false
    bool writeProtected(const QHash<QString, QString> &data, QString *errorMessage);
    bool readProtected(QHash<QString, QString> *data, QString *errorMessage) const;

    mutable QMutex m_mutex;
    SecureStoreConfig m_config;
    StorageMode m_mode = StorageMode::Unavailable;
    QString m_reason;
    QString m_riskNotice;
    bool m_needsConsent = false;
    bool m_initialized = false;
    bool m_cacheValid = false;
};

} // namespace dc
