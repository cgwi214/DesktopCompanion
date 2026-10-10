#include "SecureStore.h"

#include "core/Logger.h"
#include "core/ErrorCode.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QCryptographicHash>
#include <QSaveFile>
#include <QRandomGenerator>
#include <QStandardPaths>

#include <cstring>

#ifdef Q_OS_WIN
#  include <windows.h>
#  include <wincrypt.h>
#  ifndef CRYPTPROTECT_UI_FORBIDDEN
#    define CRYPTPROTECT_UI_FORBIDDEN 0x1
#  endif
#endif

namespace dc {

// ============================================================================
// 枚举与文本
// ============================================================================
QString storageModeToString(StorageMode mode)
{
    switch (mode) {
    case StorageMode::SystemKeyring: return QStringLiteral("system_keyring");
    case StorageMode::ProtectedFile: return QStringLiteral("protected_file");
    case StorageMode::PlaintextFile: return QStringLiteral("plaintext_file");
    case StorageMode::Unavailable:   return QStringLiteral("unavailable");
    }
    // 未知值回退到最保守的取值（SRS 设计原则：失败安全）
    return QStringLiteral("unavailable");
}

StorageMode storageModeFromString(const QString &text)
{
    const QString t = text.trimmed().toLower();
    if (t == QLatin1String("system_keyring")) return StorageMode::SystemKeyring;
    if (t == QLatin1String("protected_file")) return StorageMode::ProtectedFile;
    if (t == QLatin1String("plaintext_file")) return StorageMode::PlaintextFile;
    // 任何无法识别的字符串都不允许变成"更宽松"的级别
    return StorageMode::Unavailable;
}

QString storageModeDescription(StorageMode mode)
{
    switch (mode) {
    case StorageMode::SystemKeyring:
        return QObject::tr("密钥由操作系统的凭据保护机制加密保存，其他用户帐户无法读取。");
    case StorageMode::ProtectedFile:
        return QObject::tr("密钥经操作系统用户身份加密后保存，其他用户帐户无法读取。");
    case StorageMode::PlaintextFile:
        return QObject::tr("密钥以明文形式保存在本机文件中。任何能访问该文件的程序或用户都能读取它。");
    case StorageMode::Unavailable:
        return QObject::tr("无法在本机安全保存密钥，因此依赖密钥的功能（如在线翻译）将不可用。");
    }
    return QObject::tr("无法在本机安全保存密钥。");
}

bool storageModeIsRisky(StorageMode mode)
{
    return mode == StorageMode::PlaintextFile || mode == StorageMode::Unavailable;
}

// ============================================================================
// 构造 / 析构
// ============================================================================
SecureStore::SecureStore(QObject *parent)
    : QObject(parent)
{
}

SecureStore::~SecureStore()
{
    // 进程退出前尽量抹掉缓存中的明文密钥
    clearMemoryCache();
}

// ============================================================================
// 初始化：能力探测优先（SRS 6.2）
// ============================================================================
SecureStoreInitResult SecureStore::initialize(const SecureStoreConfig &config)
{
    QMutexLocker locker(&m_mutex);

    SecureStoreInitResult result;
    m_config = config;
    m_initialized = false;
    m_cacheValid = false;

    if (m_config.storeDirectory.trimmed().isEmpty()) {
        result.ok = false;
        result.mode = StorageMode::Unavailable;
        result.reason = tr("未指定密钥存储目录，无法初始化安全存储。");
        m_mode = StorageMode::Unavailable;
        m_reason = result.reason;
        m_riskNotice = result.reason;
        return result;
    }

    result.capabilities << tr("存储目录：%1").arg(m_config.storeDirectory);

    // --- 第一层：询问操作系统是否提供"加密到当前用户"的能力 ---
    QString dpapiDetail;
    const bool hasProtected = detectProtectedStorage(&dpapiDetail);
    if (hasProtected) {
        result.capabilities << tr("受保护存储：可用（%1）").arg(dpapiDetail);
    } else {
        result.capabilities << tr("受保护存储：不可用（%1）").arg(
            dpapiDetail.isEmpty() ? tr("未检测到操作系统级加密能力") : dpapiDetail);
    }

    // 目录能建出来才谈得上存东西
    QString dirError;
    if (!ensureDirectory(&dirError)) {
        result.ok = false;
        result.mode = StorageMode::Unavailable;
        result.reason = tr("无法创建密钥存储目录：%1").arg(dirError);
        m_mode = StorageMode::Unavailable;
        m_reason = result.reason;
        m_riskNotice = result.reason;
        DC_WARN(LogCat::Core, QStringLiteral("SecureStore 初始化失败：%1").arg(result.reason));
        return result;
    }

    // --- 第二层：选择后端 ---
    if (hasProtected) {
        // Windows DPAPI 路径。它同时满足"系统凭据保护"和"用户级文件"两种描述，
        // 这里按 SRS 的语义选择 ProtectedFile —— 相比纯 keyring 更可移植，
        // 且无需额外依赖 libsecret。若将来接入 Windows Credential Manager，
        // 只需在此处把 SystemKeyring 分支提到前面即可，接口不变。
        m_mode = StorageMode::ProtectedFile;
        result.ok = true;
        result.mode = m_mode;
        result.reason.clear();
        m_reason.clear();
        m_riskNotice.clear();
        m_needsConsent = false;
        m_initialized = true;

        DC_INFO(LogCat::Core, QStringLiteral("SecureStore 后端就绪：protected_file"));
        locker.unlock();
        emit modeChanged(m_mode);
        return result;
    }

    // 没有操作系统级保护 —— 只有两种情况：用户已知情同意，或功能不可用。
    if (m_config.allowPlaintextFallback && m_config.plaintextConsentGranted) {
        m_mode = StorageMode::PlaintextFile;
        result.ok = true;
        result.mode = m_mode;
        result.riskNotice = tr(
            "本机不支持操作系统级的密钥加密。你已选择以明文方式保存密钥：\n"
            "• 密钥会明文保存在：%1\n"
            "• 任何能读取该文件的程序或用户都可以获取它\n"
            "• 建议只使用权限受限的、可随时吊销的 API 密钥\n"
            "• 随时可以在设置中清除已保存的密钥").arg(m_config.storeDirectory);
        result.needsUserConsent = false;
        m_riskNotice = result.riskNotice;
        m_needsConsent = false;
        m_initialized = true;

        DC_WARN(LogCat::Core, QStringLiteral("SecureStore 降级为明文存储（用户已同意）"));
        locker.unlock();
        emit modeChanged(m_mode);
        emit securityNotice(result.riskNotice);
        return result;
    }

    if (!m_config.allowPlaintextFallback) {
        // 用户未开启明文回退 —— 保持不可用，依赖密钥的功能被禁用。
        m_mode = StorageMode::Unavailable;
        result.ok = false;
        result.mode = m_mode;
        result.reason = tr(
            "本机未提供操作系统级的密钥加密能力，且已禁用明文回退。"
            "依赖密钥的功能将不可用。");
        result.riskNotice = result.reason;
        m_reason = result.reason;
        m_riskNotice = result.riskNotice;
        m_needsConsent = false;
        m_initialized = true;

        DC_WARN(LogCat::Core, QStringLiteral("SecureStore 不可用：%1").arg(result.reason));
        locker.unlock();
        emit modeChanged(m_mode);
        return result;
    }

    // 开启了明文回退但还没拿到用户同意 —— 需要 UI 先问一次。
    m_mode = StorageMode::Unavailable;
    result.ok = false;
    result.mode = m_mode;
    result.needsUserConsent = true;
    result.riskNotice = tr(
        "本机不支持操作系统级的密钥加密。若继续，密钥将以明文保存在本机文件中，"
        "其他程序或用户可能读取到它。\n"
        "建议：仅在确实需要在线翻译等功能时启用，并使用可随时吊销的密钥。\n"
        "你也可以选择不保存密钥，此时相关功能不可用。");
    result.reason = tr("等待用户确认是否接受明文存储风险。");
    m_reason = result.reason;
    m_riskNotice = result.riskNotice;
    m_needsConsent = true;
    m_initialized = true;

    locker.unlock();
    emit modeChanged(m_mode);
    return result;
}

StorageMode SecureStore::mode() const
{
    QMutexLocker locker(&m_mutex);
    return m_mode;
}

bool SecureStore::isAvailable() const
{
    QMutexLocker locker(&m_mutex);
    return m_mode == StorageMode::ProtectedFile || m_mode == StorageMode::SystemKeyring;
}

bool SecureStore::needsUserConsent() const
{
    QMutexLocker locker(&m_mutex);
    return m_needsConsent;
}

QString SecureStore::riskNotice() const
{
    QMutexLocker locker(&m_mutex);
    return m_riskNotice;
}

void SecureStore::grantPlaintextConsent()
{
    QString notice;
    {
        QMutexLocker locker(&m_mutex);
        m_config.allowPlaintextFallback = true;
        m_config.plaintextConsentGranted = true;
        m_needsConsent = false;
        notice = m_riskNotice;
    }
    // 注意：模式切换由调用方重新 initialize() 完成，保证"一次探测一次生效"，
    // 避免运行期静默提升权限级别。
    emit securityNotice(notice);
}

// ============================================================================
// 唯一允许进日志的密钥标识
// ============================================================================
QString SecureStore::keyFingerprint(const QString &key)
{
    const QByteArray digest = QCryptographicHash::hash(
        key.toUtf8(), QCryptographicHash::Sha256).toHex().left(12);
    return QString::fromLatin1(digest);
}

void SecureStore::clearMemoryCache()
{
    QMutexLocker locker(&m_mutex);
    m_cacheValid = false;
}

QString SecureStore::describeBackend() const
{
    QMutexLocker locker(&m_mutex);
    switch (m_mode) {
    case StorageMode::SystemKeyring:
        return tr("系统凭据库（安全）");
    case StorageMode::ProtectedFile:
        return tr("操作系统加密文件（安全）");
    case StorageMode::PlaintextFile:
        return tr("明文文件（存在风险）");
    case StorageMode::Unavailable:
        return tr("不可用 —— %1").arg(m_reason.isEmpty() ? tr("未初始化") : m_reason);
    }
    return tr("不可用");
}

// ============================================================================
// 读写入口
// ============================================================================
bool SecureStore::setSecret(const QString &key, const QString &value, QString *errorMessage)
{
    const QString trimmedKey = key.trimmed();
    if (trimmedKey.isEmpty()) {
        if (errorMessage) *errorMessage = tr("密钥名不能为空。");
        return false;
    }

    QHash<QString, QString> data;
    StorageMode currentMode;
    {
        QMutexLocker locker(&m_mutex);
        if (!m_initialized) {
            if (errorMessage) *errorMessage = tr("安全存储尚未初始化。");
            return false;
        }
        if (m_mode == StorageMode::Unavailable) {
            if (errorMessage) {
                *errorMessage = m_reason.isEmpty()
                    ? tr("本机无法安全保存密钥，已拒绝写入。")
                    : m_reason;
            }
            return false;
        }
        currentMode = m_mode;
        m_cacheValid = false;
    }

    // 先读旧数据，保留其它键
    QString readError;
    if (!readBackend(&data, &readError)) {
        // 读失败不一定是致命问题（文件不存在是正常的），但如果是解析错误必须报出来，
        // 否则会覆盖掉用户已有的密钥。
        if (!readError.isEmpty()) {
            if (errorMessage) *errorMessage = readError;
            DC_WARN(LogCat::Core, QStringLiteral("SecureStore 读取既有数据失败，拒绝覆盖写入"));
            return false;
        }
    }

    data.insert(trimmedKey, value);

    QString writeError;
    const bool ok = writeBackend(data, &writeError);
    if (!ok) {
        if (errorMessage) *errorMessage = writeError;
        return false;
    }

    // 只记录指纹，绝不记录键名原文（键名可能含服务商名称，但值一定不能进日志）
    DC_INFO(LogCat::Core, QStringLiteral("SecureStore 已保存密钥指纹 %1（后端 %2）")
                .arg(keyFingerprint(trimmedKey), storageModeToString(currentMode)));
    return true;
}

SecretValue SecureStore::secret(const QString &key) const
{
    SecretValue result;
    result.key = key;

    QMutexLocker locker(&m_mutex);
    if (!m_initialized || m_mode == StorageMode::Unavailable) {
        return result;
    }
    result.mode = m_mode;
    locker.unlock();

    QHash<QString, QString> data;
    QString readError;
    if (!readBackend(&data, &readError)) {
        if (!readError.isEmpty()) {
            DC_WARN(LogCat::Core, QStringLiteral("SecureStore 读取失败（指纹 %1）").arg(keyFingerprint(key)));
        }
        return result;
    }

    const auto it = data.constFind(key.trimmed());
    if (it == data.constEnd()) {
        return result;
    }

    result.value = it.value();
    result.found = true;
    return result;
}

bool SecureStore::contains(const QString &key) const
{
    QMutexLocker locker(&m_mutex);
    if (!m_initialized || m_mode == StorageMode::Unavailable) {
        return false;
    }
    locker.unlock();

    QHash<QString, QString> data;
    QString readError;
    if (!readBackend(&data, &readError)) {
        return false;
    }
    return data.contains(key.trimmed());
}

bool SecureStore::removeSecret(const QString &key, QString *errorMessage)
{
    QHash<QString, QString> data;
    {
        QMutexLocker locker(&m_mutex);
        if (!m_initialized || m_mode == StorageMode::Unavailable) {
            if (errorMessage) *errorMessage = tr("安全存储不可用。");
            return false;
        }
        m_cacheValid = false;
    }

    QString readError;
    if (!readBackend(&data, &readError)) {
        if (!readError.isEmpty()) {
            if (errorMessage) *errorMessage = readError;
            return false;
        }
        return true; // 没有文件 = 没有这个键，视为删除成功
    }

    if (!data.remove(key.trimmed())) {
        return true; // 本来就不存在
    }

    QString writeError;
    if (!writeBackend(data, &writeError)) {
        if (errorMessage) *errorMessage = writeError;
        return false;
    }
    DC_INFO(LogCat::Core, QStringLiteral("SecureStore 已删除密钥指纹 %1").arg(keyFingerprint(key)));
    return true;
}

bool SecureStore::clearAll(QString *errorMessage)
{
    {
        QMutexLocker locker(&m_mutex);
        if (!m_initialized || m_mode == StorageMode::Unavailable) {
            if (errorMessage) *errorMessage = tr("安全存储不可用。");
            return false;
        }
        m_cacheValid = false;
    }

    QString writeError;
    if (!writeBackend(QHash<QString, QString>(), &writeError)) {
        if (errorMessage) *errorMessage = writeError;
        return false;
    }
    DC_INFO(LogCat::Core, QStringLiteral("SecureStore 已清空全部密钥"));
    return true;
}

QStringList SecureStore::keys() const
{
    QMutexLocker locker(&m_mutex);
    if (!m_initialized || m_mode == StorageMode::Unavailable) {
        return QStringList();
    }
    locker.unlock();

    QHash<QString, QString> data;
    QString readError;
    if (!readBackend(&data, &readError)) {
        return QStringList();
    }
    QStringList list = data.keys();
    list.sort();
    return list;
}

// ============================================================================
// 后端调度
// ============================================================================
QString SecureStore::backendFilePath() const
{
    QMutexLocker locker(&m_mutex);
    return QDir(m_config.storeDirectory).filePath(QStringLiteral("secrets.dat"));
}

bool SecureStore::ensureDirectory(QString *errorMessage) const
{
    // 注意：storeDirectory 是 POD 配置字段，直接从 m_config 读即可，
    // 千万不要在这里再锁一次 m_mutex。
    //
    // 这里踩过一个很难查的死锁：initialize() 一进来就
    //     QMutexLocker locker(&m_mutex);
    // 然后在持有锁的状态下调用本函数。而本函数原先又写了一遍
    //     QMutexLocker locker(&m_mutex);
    // QMutex 默认是**非递归**的，同一线程重复加锁会永久阻塞 ——
    // 现象是程序启动到一半没有任何输出、进程还在、CPU 几乎为 0，
    // 也没有崩溃转储，非常容易误判成"I/O 卡住"或"DPAPI 阻塞"。
    // 修法是让本函数假定调用方已持锁（它本来就是 initialize 的私有步骤，
    // 外部调用点也都持锁），不再自己加锁。
    const QString dirPath = m_config.storeDirectory;

    QDir dir(dirPath);
    if (dir.exists()) {
        return true;
    }
    if (!dir.mkpath(QStringLiteral("."))) {
        if (errorMessage) *errorMessage = dirPath;
        return false;
    }

#ifndef Q_OS_WIN
    // POSIX：把目录收紧到仅属主可访问
    QFile::setPermissions(dirPath,
                          QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
#endif
    return true;
}

bool SecureStore::writeBackend(const QHash<QString, QString> &data, QString *errorMessage)
{
    StorageMode currentMode;
    {
        QMutexLocker locker(&m_mutex);
        currentMode = m_mode;
    }

    switch (currentMode) {
    case StorageMode::SystemKeyring:
    case StorageMode::ProtectedFile:
        return writeProtected(data, errorMessage);
    case StorageMode::PlaintextFile:
        return writePlaintext(data, errorMessage);
    case StorageMode::Unavailable:
        break;
    }
    if (errorMessage) *errorMessage = tr("安全存储不可用，已拒绝写入密钥。");
    return false;
}

bool SecureStore::readBackend(QHash<QString, QString> *data, QString *errorMessage) const
{
    StorageMode currentMode;
    {
        QMutexLocker locker(&m_mutex);
        currentMode = m_mode;
    }

    switch (currentMode) {
    case StorageMode::SystemKeyring:
    case StorageMode::ProtectedFile:
        return readProtected(data, errorMessage);
    case StorageMode::PlaintextFile:
        return readPlaintext(data, errorMessage);
    case StorageMode::Unavailable:
        break;
    }
    return true; // 不可用时返回空集合，不算错误
}

// ============================================================================
// 明文后端
// ============================================================================
bool SecureStore::writePlaintext(const QHash<QString, QString> &data, QString *errorMessage)
{
    QJsonObject root;
    root.insert(QStringLiteral("format"), QStringLiteral("dc-secrets-plain"));
    root.insert(QStringLiteral("version"), 1);

    QJsonObject secrets;
    for (auto it = data.constBegin(); it != data.constEnd(); ++it) {
        secrets.insert(it.key(), it.value());
    }
    root.insert(QStringLiteral("secrets"), secrets);

    const QString path = backendFilePath();

    // 原子写入：先写临时文件再替换，避免断电/崩溃留下半个文件
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (errorMessage) *errorMessage = tr("无法写入密钥文件：%1").arg(path);
        return false;
    }
    file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    if (!file.commit()) {
        if (errorMessage) *errorMessage = tr("保存密钥文件失败：%1").arg(path);
        return false;
    }

#ifndef Q_OS_WIN
    QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner);
#endif
    return true;
}

bool SecureStore::readPlaintext(QHash<QString, QString> *data, QString *errorMessage) const
{
    const QString path = backendFilePath();
    QFile file(path);
    if (!file.exists()) {
        return true; // 空集合
    }
    if (!file.open(QIODevice::ReadOnly)) {
        if (errorMessage) *errorMessage = tr("无法读取密钥文件：%1").arg(path);
        return false;
    }

    QJsonParseError parseError{};
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parseError);
    file.close();

    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        if (errorMessage) {
            *errorMessage = tr("密钥文件已损坏：%1（第 %2 字节）")
                                .arg(parseError.errorString())
                                .arg(parseError.offset);
        }
        return false;
    }

    const QJsonObject secrets = doc.object().value(QStringLiteral("secrets")).toObject();
    for (auto it = secrets.constBegin(); it != secrets.constEnd(); ++it) {
        if (it.value().isString()) {
            data->insert(it.key(), it.value().toString());
        }
    }
    return true;
}

// ============================================================================
// 受保护后端
// ============================================================================
bool SecureStore::detectProtectedStorage(QString *detail)
{
#ifdef Q_OS_WIN
    // 探测 DPAPI 是否真的能用：用一个极短的缓冲区跑一遍加解密往返。
    // 只看 API 是否存在是不够的 —— 某些受限环境下 CryptProtectData 会失败。
    DATA_BLOB in{};
    const char probe[] = "dc";
    in.pbData = reinterpret_cast<BYTE *>(const_cast<char *>(probe));
    in.cbData = static_cast<DWORD>(sizeof(probe) - 1);

    DATA_BLOB out{};
    const BOOL protectedOk = CryptProtectData(
        &in, L"dc-probe", nullptr, nullptr, nullptr,
        CRYPTPROTECT_UI_FORBIDDEN, &out);

    if (!protectedOk) {
        if (detail) {
            *detail = QStringLiteral("CryptProtectData 失败（错误码 %1）")
                          .arg(static_cast<quint32>(GetLastError()));
        }
        return false;
    }

    DATA_BLOB plain{};
    const BOOL unprotectedOk = CryptUnprotectData(
        &out, nullptr, nullptr, nullptr, nullptr,
        CRYPTPROTECT_UI_FORBIDDEN, &plain);

    if (out.pbData) {
        LocalFree(out.pbData);
    }
    if (!unprotectedOk) {
        if (detail) {
            *detail = QStringLiteral("CryptUnprotectData 失败（错误码 %1）")
                          .arg(static_cast<quint32>(GetLastError()));
        }
        return false;
    }

    const bool roundTripOk = (plain.cbData == in.cbData)
        && (std::memcmp(plain.pbData, in.pbData, in.cbData) == 0);
    if (plain.pbData) {
        LocalFree(plain.pbData);
    }

    if (!roundTripOk) {
        if (detail) *detail = QStringLiteral("DPAPI 往返校验不一致");
        return false;
    }

    if (detail) *detail = QStringLiteral("Windows DPAPI 可用");
    return true;
#else
    // Linux 侧本实现不引入 libsecret 依赖（SRS 要求模块可替换、依赖可控）。
    // 若发行版提供安全存储且后续需要接入，只需在此处探测并返回 true，
    // 上层逻辑无需改动。
    if (detail) *detail = QStringLiteral("当前平台实现未启用系统级加密后端");
    return false;
#endif
}

bool SecureStore::writeProtected(const QHash<QString, QString> &data, QString *errorMessage)
{
#ifdef Q_OS_WIN
    QJsonObject root;
    root.insert(QStringLiteral("format"), QStringLiteral("dc-secrets-1"));
    root.insert(QStringLiteral("version"), 1);

    QJsonObject secrets;
    for (auto it = data.constBegin(); it != data.constEnd(); ++it) {
        secrets.insert(it.key(), it.value());
    }
    root.insert(QStringLiteral("secrets"), secrets);

    const QByteArray plaintext = QJsonDocument(root).toJson(QJsonDocument::Compact);

    DATA_BLOB in{};
    in.pbData = reinterpret_cast<BYTE *>(const_cast<char *>(plaintext.constData()));
    in.cbData = static_cast<DWORD>(plaintext.size());

    DATA_BLOB out{};
    if (!CryptProtectData(&in, L"DesktopCompanion", nullptr, nullptr, nullptr,
                          CRYPTPROTECT_UI_FORBIDDEN, &out)) {
        if (errorMessage) {
            *errorMessage = tr("系统加密失败（错误码 %1），密钥未保存。")
                                .arg(static_cast<quint32>(GetLastError()));
        }
        return false;
    }

    const QByteArray cipher(reinterpret_cast<const char *>(out.pbData),
                            static_cast<int>(out.cbData));
    LocalFree(out.pbData);

    const QString path = backendFilePath();
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (errorMessage) *errorMessage = tr("无法写入密钥文件：%1").arg(path);
        return false;
    }
    file.write(cipher);
    if (!file.commit()) {
        if (errorMessage) *errorMessage = tr("保存密钥文件失败：%1").arg(path);
        return false;
    }

    // 明文缓冲区用完立刻清零，减少残留在内存中的时间
    if (!plaintext.isEmpty()) {
        volatile char *p = const_cast<volatile char *>(plaintext.constData());
        for (int i = 0; i < plaintext.size(); ++i) {
            p[i] = 0;
        }
    }
    return true;
#else
    Q_UNUSED(data);
    if (errorMessage) *errorMessage = tr("当前平台不支持系统级加密存储。");
    return false;
#endif
}

bool SecureStore::readProtected(QHash<QString, QString> *data, QString *errorMessage) const
{
#ifdef Q_OS_WIN
    const QString path = backendFilePath();
    QFile file(path);
    if (!file.exists()) {
        return true; // 还没有保存任何密钥
    }
    if (!file.open(QIODevice::ReadOnly)) {
        if (errorMessage) *errorMessage = tr("无法读取密钥文件：%1").arg(path);
        return false;
    }
    const QByteArray cipher = file.readAll();
    file.close();

    if (cipher.isEmpty()) {
        return true;
    }

    DATA_BLOB in{};
    in.pbData = reinterpret_cast<BYTE *>(const_cast<char *>(cipher.constData()));
    in.cbData = static_cast<DWORD>(cipher.size());

    DATA_BLOB out{};
    if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr,
                            CRYPTPROTECT_UI_FORBIDDEN, &out)) {
        const DWORD err = GetLastError();
        // 换用户、换机器、文件被改写都会导致解密失败。
        // 明确告知用户"密钥需要重新输入"，而不是假装成功。
        if (errorMessage) {
            *errorMessage = tr(
                "无法解密已保存的密钥（错误码 %1）。"
                "可能是由于文件被其他用户或机器修改，或系统用户环境发生变化。"
                "请在设置中重新输入密钥。").arg(static_cast<quint32>(err));
        }
        DC_WARN(LogCat::Core, QStringLiteral("SecureStore 解密失败，错误码 %1").arg(static_cast<quint32>(err)));
        return false;
    }

    const QByteArray plaintext(reinterpret_cast<const char *>(out.pbData),
                               static_cast<int>(out.cbData));
    LocalFree(out.pbData);

    QJsonParseError parseError{};
    const QJsonDocument doc = QJsonDocument::fromJson(plaintext, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        if (errorMessage) {
            *errorMessage = tr("已解密的密钥数据结构无效：%1").arg(parseError.errorString());
        }
        return false;
    }

    const QJsonObject secrets = doc.object().value(QStringLiteral("secrets")).toObject();
    for (auto it = secrets.constBegin(); it != secrets.constEnd(); ++it) {
        if (it.value().isString()) {
            data->insert(it.key(), it.value().toString());
        }
    }
    return true;
#else
    Q_UNUSED(data);
    if (errorMessage) *errorMessage = tr("当前平台不支持系统级加密存储。");
    return false;
#endif
}

} // namespace dc
