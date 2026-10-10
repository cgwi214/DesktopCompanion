// ============================================================================
//  PetRenderer.cpp — 桌宠形象渲染实现
//
//  需求依据：SRS V1.1 4.1（PET-001 ~ PET-007）、7.1（低占用）
//
//  性能考虑（SRS 7.1 明确要求低占用）：
//    1. 所有帧在加载时就按当前 scale 缩放好并缓存，
//       绘制时直接 blit，不做任何实时缩放。
//       —— 实时缩放是桌宠类程序最常见的 CPU 热点。
//    2. 只在用户改缩放时整批重建（rebuildCache），不是每帧。
//    3. 帧数上限保护：单个状态最多 120 帧，
//       防止用户误指向一个几千张图的目录把内存吃光。
//    4. 单张图尺寸上限 2048px，超过则拒绝并给出警告
//       （SRS 7.3：所有外部输入均需校验）。
// ============================================================================
#include "ui/PetRenderer.h"

#include "core/Logger.h"
#include "platform/PathGuard.h"

#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPainter>
#include <QPainterPath>
#include <QRegularExpression>

namespace dc {

// ---------------------------------------------------------------------------
// 资源限制（集中定义，便于核对与调整）
// ---------------------------------------------------------------------------
namespace {
// 单个状态最多加载多少帧。超过就截断并警告。
// 200ms/帧时 120 帧相当于 24 秒动画，对桌宠足够。
constexpr int kMaxFramesPerState = 120;

// 单张图的边长上限。超过说明用户选错了文件（例如一张 4K 照片）。
constexpr int kMaxImageSide = 2048;

// 支持的图片扩展名
const QStringList kImageExtensions = {
    QStringLiteral("png"), QStringLiteral("jpg"), QStringLiteral("jpeg"),
    QStringLiteral("bmp"), QStringLiteral("webp"), QStringLiteral("gif")
};
}   // namespace

QString petAssetKindToString(PetAssetKind kind)
{
    switch (kind) {
    case PetAssetKind::BuiltinPlaceholder: return QStringLiteral("builtin");
    case PetAssetKind::StaticImage:        return QStringLiteral("static");
    case PetAssetKind::FrameSequence:      return QStringLiteral("frames");
    }
    return QStringLiteral("builtin");
}

// ---------------------------------------------------------------------------
// 静态：状态 -> 目录名
// ---------------------------------------------------------------------------
QString PetRenderer::stateDirectoryName(PetState state)
{
    switch (state) {
    case PetState::Idle:      return QStringLiteral("idle");
    case PetState::Talking:   return QStringLiteral("talking");
    case PetState::Working:   return QStringLiteral("working");
    case PetState::Success:   return QStringLiteral("success");
    case PetState::Failure:   return QStringLiteral("failure");
    case PetState::Listening: return QStringLiteral("listening");
    }
    return QStringLiteral("idle");
}

// 全部状态的固定顺序（用于遍历，避免用到 QMap 的隐式排序）
static const PetState kAllStates[] = {
    PetState::Idle, PetState::Talking, PetState::Working,
    PetState::Success, PetState::Failure, PetState::Listening
};

// ---------------------------------------------------------------------------
// 构造
// ---------------------------------------------------------------------------
PetRenderer::PetRenderer(QObject *parent)
    : QObject(parent)
{
}

PetRenderer::~PetRenderer() = default;

// ---------------------------------------------------------------------------
// 配置与加载
// ---------------------------------------------------------------------------
int PetRenderer::setConfig(const PetRenderConfig &config)
{
    m_config = config;

    // scale 做边界保护。SRS 5 规定 0.5 ~ 3.0。
    // 这里再夹一次而不是信任配置，因为配置文件可以被手工改坏。
    if (m_config.scale < 0.5) {
        m_config.scale = 0.5;
    }
    if (m_config.scale > 3.0) {
        m_config.scale = 3.0;
    }

    m_frames.clear();
    m_originalSizes.clear();
    m_frameIntervals.clear();
    m_loadedStates.clear();
    m_loadWarnings.clear();

    int loadedGroups = 0;

    switch (m_config.kind) {
    case PetAssetKind::StaticImage:
        if (loadStaticImage(m_config.staticImagePath)) {
            loadedGroups = 1;
        } else {
            m_loadWarnings
                << tr("指定的形象图片无法加载：%1")
                       .arg(PathGuard::redact(m_config.staticImagePath));
        }
        break;

    case PetAssetKind::FrameSequence: {
        // 用户在设置里指定了一个动画目录。
        // 该目录下按状态分文件夹；缺哪个状态就用 idle 顶替。
        const bool manifestOk = loadSkinManifest(m_config.animationDir);
        if (!manifestOk) {
            // skin.json 不是必需的，缺失只是没有自定义帧间隔
            DC_INFO(LogCat::Ui, QStringLiteral("动画目录没有 skin.json，使用默认参数"));
        }

        for (PetState state : kAllStates) {
            const QString dir =
                QDir(m_config.animationDir).filePath(stateDirectoryName(state));
            if (loadStateFrames(state, dir)) {
                ++loadedGroups;
            }
        }
        break;
    }

    case PetAssetKind::BuiltinPlaceholder:
    default:
        // 内置皮肤：尝试从 skinsRoot/<skinId>/ 读，
        // 读不到就用代码绘制的占位形象（这不是错误）。
        if (!m_config.skinsRoot.isEmpty()) {
            const QString skinDir =
                QDir(m_config.skinsRoot).filePath(m_config.skinId);

            if (QFileInfo::exists(skinDir)) {
                loadSkinManifest(skinDir);
                for (PetState state : kAllStates) {
                    const QString dir =
                        QDir(skinDir).filePath(stateDirectoryName(state));
                    if (loadStateFrames(state, dir)) {
                        ++loadedGroups;
                    }
                }
            }
        }
        break;
    }

    // ---- 补齐缺失的状态 ----
    //  关键：任何缺失的状态用 idle 的帧顶替。
    //  这样状态机切到 Success 时不会突然什么都没了（视觉上会闪一下）。
    const QList<QPixmap> idleFrames =
        m_frames.value(static_cast<int>(PetState::Idle));
    const QSize idleSize = m_originalSizes.value(static_cast<int>(PetState::Idle));

    for (PetState state : kAllStates) {
        const int key = static_cast<int>(state);
        if (m_frames.value(key).isEmpty()) {
            if (!idleFrames.isEmpty()) {
                m_frames.insert(key, idleFrames);
                m_originalSizes.insert(key, idleSize);
                m_frameIntervals.insert(key, 200);
            }
        }
    }

    // ---- 计算显示尺寸 ----
    //  取所有状态里最大的那一帧。窗口按它固定，
    //  状态切换时窗口不跳动，视觉更稳。
    m_displaySize = QSize();
    for (PetState state : kAllStates) {
        const QSize original = m_originalSizes.value(static_cast<int>(state));
        if (original.isValid()) {
            const QSize scaled = scaledSize(original);
            m_displaySize.setWidth(qMax(m_displaySize.width(), scaled.width()));
            m_displaySize.setHeight(qMax(m_displaySize.height(), scaled.height()));
        }
    }

    m_usingPlaceholder = (loadedGroups == 0);

    if (m_usingPlaceholder) {
        // PET-007：资源缺失时用内置占位形象，并给出警告。
        // 用占位形象时窗口尺寸取一个合理默认值。
        const int base = qRound(160.0 * m_config.scale);
        m_displaySize = QSize(base, base);
        m_loadWarnings << tr("没有找到形象资源，正在使用内置占位形象。");
    } else {
        for (PetState state : kAllStates) {
            if (!m_frames.value(static_cast<int>(state)).isEmpty()) {
                m_loadedStates << stateDirectoryName(state);
            }
        }
    }

    DC_INFO(LogCat::Ui,
            QStringLiteral("形象渲染就绪：来源=%1 已加载状态组=%2 显示尺寸=%3x%4%5")
                .arg(petAssetKindToString(m_config.kind))
                .arg(loadedGroups)
                .arg(m_displaySize.width())
                .arg(m_displaySize.height())
                .arg(m_usingPlaceholder ? QStringLiteral("（占位形象）") : QString()));

    emit assetsReloaded();
    return loadedGroups;
}

PetRenderConfig PetRenderer::config() const
{
    return m_config;
}

// ---------------------------------------------------------------------------
// 静态图片
// ---------------------------------------------------------------------------
bool PetRenderer::loadStaticImage(const QString &path)
{
    if (path.isEmpty() || !QFileInfo::exists(path)) {
        return false;
    }

    // 用 QImageReader 而不是直接 QPixmap::load：
    // 前者能在解码前检查尺寸，避免被一张超大图把内存吃光。
    QImageReader reader(path);
    reader.setAutoTransform(true);   // 尊重 EXIF 旋转（手机照片常见）

    // 静态图只取第一帧。GIF 也不做动画 —— SRS 4.1 PET-001
    // 明确说"GIF 首帧"，因为做 GIF 播放会引入计时与解码开销，
    // 而用户想要动画的话应该用序列帧（PET-002）。
    const QImage image = reader.read();

    if (image.isNull()) {
        DC_WARN(LogCat::Ui,
                QStringLiteral("形象图片解码失败：%1（%2）")
                    .arg(QFileInfo(path).fileName(), reader.errorString()));
        return false;
    }

    if (image.width() > kMaxImageSide || image.height() > kMaxImageSide) {
        m_loadWarnings << tr("形象图片尺寸过大（%1x%2），已拒绝加载。"
                             "建议使用 %3 像素以内的图片。")
                              .arg(image.width())
                              .arg(image.height())
                              .arg(kMaxImageSide);
        return false;
    }

    const QPixmap source = QPixmap::fromImage(image);
    const QPixmap cached = scaled(source);

    // 静态图复制到全部状态。
    // 状态机仍然会跑，但因为帧数都是 1，
    // tick() 不会有帧推进 —— 也就是"静态图模式下只做有限反馈"
    //（PET-006：状态变了但没有动画，只有窗口内容轻微变化，
    //  由 PetWindow 用描边/透明度做反馈）。
    for (PetState state : kAllStates) {
        const int key = static_cast<int>(state);
        m_frames.insert(key, QList<QPixmap>{cached});
        m_originalSizes.insert(key, source.size());
        m_frameIntervals.insert(key, 0);   // 0 = 不需要推进帧
    }

    return true;
}

// ---------------------------------------------------------------------------
// 序列帧
// ---------------------------------------------------------------------------
bool PetRenderer::loadStateFrames(PetState state, const QString &directory)
{
    QDir dir(directory);
    if (!dir.exists()) {
        return false;
    }

    // 收集目录下所有支持的图片，按文件名排序。
    // 按名字排序而不是按修改时间：用户把 000.png ~ 011.png 命名好，
    // 顺序就应该由名字决定，重命名一下不该让动画顺序乱掉。
    const QFileInfoList entries = dir.entryInfoList(
        QStringList(), QDir::Files | QDir::Readable, QDir::Name);

    QStringList framePaths;
    for (const QFileInfo &info : entries) {
        const QString suffix = info.suffix().toLower();
        if (kImageExtensions.contains(suffix)) {
            framePaths << info.absoluteFilePath();
        }
        if (framePaths.size() >= kMaxFramesPerState) {
            m_loadWarnings
                << tr("状态「%1」的图片超过 %2 张，只加载了前 %2 张。")
                       .arg(stateDirectoryName(state))
                       .arg(kMaxFramesPerState);
            break;
        }
    }

    if (framePaths.isEmpty()) {
        return false;
    }

    QList<QPixmap> frames;
    frames.reserve(framePaths.size());

    QSize firstOriginalSize;
    int skipped = 0;

    for (const QString &path : framePaths) {
        QImageReader reader(path);
        reader.setAutoTransform(true);

        // 解码前先看尺寸声明，避免超大图进入内存
        const QSize declared = reader.size();
        if (declared.isValid()
            && (declared.width() > kMaxImageSide || declared.height() > kMaxImageSide)) {
            ++skipped;
            continue;
        }

        const QImage image = reader.read();
        if (image.isNull()) {
            // 单张图坏掉不该让整组失败 —— 跳过并记录。
            // 这是"部分降级"：动画少一帧比完全没动画好。
            ++skipped;
            DC_WARN(LogCat::Ui,
                    QStringLiteral("序列帧解码失败已跳过：%1")
                        .arg(QFileInfo(path).fileName()));
            continue;
        }

        if (!firstOriginalSize.isValid()) {
            firstOriginalSize = image.size();
        }

        frames << scaled(QPixmap::fromImage(image));
    }

    if (frames.isEmpty()) {
        return false;
    }

    if (skipped > 0) {
        m_loadWarnings
            << tr("状态「%1」有 %2 张图片无法读取，已跳过。")
                   .arg(stateDirectoryName(state))
                   .arg(skipped);
    }

    const int key = static_cast<int>(state);
    m_frames.insert(key, frames);
    m_originalSizes.insert(key, firstOriginalSize);

    // 帧间隔：优先用配置覆盖，其次用 skin.json 的，最后默认 200ms
    int interval = m_config.frameIntervalOverrideMs;
    if (interval <= 0) {
        interval = m_frameIntervals.value(key, 0);
    }
    if (interval <= 0) {
        interval = 200;
    }
    m_frameIntervals.insert(key, interval);

    return true;
}

// ---------------------------------------------------------------------------
// skin.json
// ---------------------------------------------------------------------------
bool PetRenderer::loadSkinManifest(const QString &directory)
{
    const QString path = QDir(directory).filePath(QStringLiteral("skin.json"));
    if (!QFileInfo::exists(path)) {
        return false;
    }

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return false;
    }

    QJsonParseError parseError{};
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parseError);

    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        m_loadWarnings << tr("skin.json 格式错误：%1").arg(parseError.errorString());
        return false;
    }

    const QJsonObject root = doc.object();
    const QJsonObject animations = root.value(QStringLiteral("animations")).toObject();

    // 可选的整组帧间隔（毫秒）
    if (animations.isEmpty()) {
        return true;
    }

    for (PetState state : kAllStates) {
        const QString name = stateDirectoryName(state);
        const QJsonObject spec = animations.value(name).toObject();
        if (spec.isEmpty()) {
            continue;
        }

        // 帧间隔
        const int interval = spec.value(QStringLiteral("frameIntervalMs")).toInt(0);
        if (interval > 0) {
            const int key = static_cast<int>(state);
            m_frameIntervals.insert(key, interval);
        }

        // 帧数是"声明"，实际以目录里找到的文件数为准。
        // 声明与实际不一致时以实际为准并提示 ——
        // 因为渲染必须画真实存在的图，不能按声明去猜文件名。
        const int declaredFrames = spec.value(QStringLiteral("frameCount")).toInt(0);
        if (declaredFrames > 0) {
            const int actual = m_frames.value(static_cast<int>(state)).size();
            if (actual > 0 && actual != declaredFrames) {
                m_loadWarnings
                    << tr("状态「%1」声明 %2 帧，实际找到 %3 张图，以实际为准。")
                           .arg(name)
                           .arg(declaredFrames)
                           .arg(actual);
            }
        }
    }

    return true;
}

// ---------------------------------------------------------------------------
// 缩放
// ---------------------------------------------------------------------------
QPixmap PetRenderer::scaled(const QPixmap &source) const
{
    if (source.isNull()) {
        return QPixmap();
    }

    const QSize target = scaledSize(source.size());
    if (target == source.size()) {
        return source;   // 不需要缩放，直接用（省一次拷贝）
    }

    return source.scaled(target,
                         Qt::KeepAspectRatio,
                         m_config.transformMode);
}

QSize PetRenderer::scaledSize(const QSize &original) const
{
    if (!original.isValid()) {
        return QSize();
    }

    return QSize(qMax(1, qRound(original.width() * m_config.scale)),
                 qMax(1, qRound(original.height() * m_config.scale)));
}

void PetRenderer::rebuildCache()
{
    // 保留原始尺寸信息，重新按当前 scale 缩放。
    // 这里不重新读盘 —— 原始 QPixmap 没有留存，
    // 所以直接把已缓存的图当作源再缩一次会有累积失真问题。
    // 因此正确做法是重新走一遍 setConfig（重新读盘）。
    //
    // 说明：为了避免累积失真，本函数按当前配置重新加载。
    const PetRenderConfig saved = m_config;
    setConfig(saved);
}

// ---------------------------------------------------------------------------
// 取帧
// ---------------------------------------------------------------------------
PetFrame PetRenderer::frame(PetState state, int frameIndex) const
{
    PetFrame result;

    const int key = static_cast<int>(state);
    const QList<QPixmap> frames = m_frames.value(key);

    if (!frames.isEmpty()) {
        // 帧索引用取模而不是夹取：
        // 状态机可能因为配置不同步报出一个越界索引，
        // 取模能让它继续循环播放，而不是卡在最后一帧。
        const int index = frames.size() > 0
                              ? ((frameIndex % frames.size()) + frames.size()) % frames.size()
                              : 0;
        result.pixmap = frames.at(index);
        result.valid = !result.pixmap.isNull();
        result.logicalSize = m_originalSizes.value(key);
    }

    if (!result.isValid()) {
        // 兜底：内置占位形象。
        // 走这条路时调用方不需要做任何特殊处理（PET-007）。
        const int size = m_displaySize.isValid()
                             ? qMax(m_displaySize.width(), m_displaySize.height())
                             : qRound(160.0 * m_config.scale);
        result.pixmap = drawPlaceholder(state, frameIndex, size);
        result.valid = !result.pixmap.isNull();
        result.logicalSize = QSize(size, size);
    }

    return result;
}

int PetRenderer::frameCount(PetState state) const
{
    const int count = m_frames.value(static_cast<int>(state)).size();
    return qMax(1, count);   // 至少 1（占位形象也是 1 帧）
}

int PetRenderer::frameInterval(PetState state) const
{
    const int interval = m_frameIntervals.value(static_cast<int>(state), 200);
    // 静图时 interval 是 0，但状态机需要一个正值来推进 tick，
    // 这里给一个保守值，实际不会有帧推进（frameCount == 1）。
    return interval > 0 ? interval : 200;
}

bool PetRenderer::usingPlaceholder() const
{
    return m_usingPlaceholder;
}

QStringList PetRenderer::loadedStates() const
{
    return m_loadedStates;
}

QStringList PetRenderer::loadWarnings() const
{
    return m_loadWarnings;
}

QSize PetRenderer::displaySize() const
{
    return m_displaySize;
}

// ---------------------------------------------------------------------------
// 交给状态机
// ---------------------------------------------------------------------------
PetStateMachineConfig PetRenderer::toStateMachineConfig() const
{
    PetStateMachineConfig config;

    for (PetState state : kAllStates) {
        AnimationSpec spec;
        spec.name = stateDirectoryName(state);
        spec.frameCount = frameCount(state);
        spec.frameIntervalMs = frameInterval(state);

        // Success / Failure 不循环：停在最后一帧能保持住表情。
        // 其余状态循环。
        spec.loop = (state != PetState::Success && state != PetState::Failure);

        config.animations.insert(spec.name, spec);
    }

    // 静态图模式下只做有限反馈（PET-006）
    config.staticImageMode = (m_config.kind == PetAssetKind::StaticImage);

    return config;
}

// ===========================================================================
//  内置占位形象
//
//  设计意图（PET-007：资源缺失时不得显示破图）：
//    用 QPainter 画一个简单但**看得出情绪**的圆脸。
//    六个状态对应六种表情，这样即使完全没有美术资源，
//    用户也能一眼看出"它现在在干活 / 干完了 / 出错了 / 在听"。
//
//  为什么不做成更复杂的图形：
//    占位形象的存在意义是"能用"，不是"好看"。
//    画得越复杂，每次缺资源时的绘制开销越大。
//    这里全部是基础图形，绘制成本极低且必然成功。
// ===========================================================================
QPixmap PetRenderer::drawPlaceholder(PetState state, int frameIndex, int sizePx)
{
    const int size = qBound(32, sizePx, 1024);

    QPixmap canvas(size, size);
    canvas.fill(Qt::transparent);

    QPainter painter(&canvas);
    painter.setRenderHint(QPainter::Antialiasing, true);

    // ---- 配色（每个状态一个主色，便于区分）----
    QColor bodyColor;
    QColor faceColor = QColor(0x2B, 0x2B, 0x2B);   // 五官统一深灰

    switch (state) {
    case PetState::Idle:      bodyColor = QColor(0x7E, 0xC8, 0xE3); break;  // 天蓝
    case PetState::Talking:   bodyColor = QColor(0xF6, 0xC1, 0x77); break;  // 暖黄
    case PetState::Working:   bodyColor = QColor(0x9B, 0x8A, 0xD9); break;  // 紫
    case PetState::Success:   bodyColor = QColor(0x8B, 0xD4, 0x92); break;  // 绿
    case PetState::Failure:   bodyColor = QColor(0xE8, 0x8B, 0x8B); break;  // 红
    case PetState::Listening: bodyColor = QColor(0x6F, 0xD1, 0xC5); break;  // 青
    }

    // ---- 身体（圆）+ 轻微呼吸效果 ----
    //  用 frameIndex 让占位形象也有"呼吸"，看起来不那么死板。
    //  幅度很小（±1.5%），不会让人觉得在抖。
    const double breath = 1.0 + 0.015 * ((frameIndex % 4) - 1.5) / 1.5;

    const double margin = size * 0.06;
    QRectF bodyRect(margin, margin, size - margin * 2, size - margin * 2);

    const QPointF center = bodyRect.center();
    bodyRect.setWidth(bodyRect.width() * breath);
    bodyRect.setHeight(bodyRect.height() * breath);
    bodyRect.moveCenter(center);

    painter.setPen(Qt::NoPen);
    painter.setBrush(bodyColor);
    painter.drawEllipse(bodyRect);

    // 描边让形象在任何桌面背景上都看得清
    painter.setPen(QPen(bodyColor.darker(140), qMax(1.0, size * 0.012)));
    painter.setBrush(Qt::NoBrush);
    painter.drawEllipse(bodyRect);

    // ---- 五官 ----
    const double eyeRadius = size * 0.055;
    const double eyeY = bodyRect.center().y() - size * 0.06;
    const double eyeOffsetX = size * 0.11;

    painter.setBrush(faceColor);
    painter.setPen(Qt::NoPen);

    const auto drawEye = [&](double cx, double cy, double r) {
        painter.drawEllipse(QPointF(cx, cy), r, r);
    };

    const double leftEyeX = bodyRect.center().x() - eyeOffsetX;
    const double rightEyeX = bodyRect.center().x() + eyeOffsetX;

    switch (state) {
    case PetState::Idle:
        // 平静的圆眼
        drawEye(leftEyeX, eyeY, eyeRadius);
        drawEye(rightEyeX, eyeY, eyeRadius);
        break;

    case PetState::Talking:
        // 说话：嘴巴一张一合，用帧索引驱动
        drawEye(leftEyeX, eyeY, eyeRadius);
        drawEye(rightEyeX, eyeY, eyeRadius);
        break;

    case PetState::Working:
        // 专注：眼睛画成短线（眯眼）
        painter.setPen(QPen(faceColor, qMax(1.5, size * 0.022),
                            Qt::SolidLine, Qt::RoundCap));
        painter.drawLine(QPointF(leftEyeX - eyeRadius, eyeY),
                         QPointF(leftEyeX + eyeRadius, eyeY));
        painter.drawLine(QPointF(rightEyeX - eyeRadius, eyeY),
                         QPointF(rightEyeX + eyeRadius, eyeY));
        painter.setPen(Qt::NoPen);
        break;

    case PetState::Success:
        // 开心：眼睛画成向上的弧
        painter.setPen(QPen(faceColor, qMax(1.5, size * 0.022),
                            Qt::SolidLine, Qt::RoundCap));
        painter.drawArc(QRectF(leftEyeX - eyeRadius, eyeY - eyeRadius,
                              eyeRadius * 2, eyeRadius * 2),
                       0 * 16, 180 * 16);
        painter.drawArc(QRectF(rightEyeX - eyeRadius, eyeY - eyeRadius,
                               eyeRadius * 2, eyeRadius * 2),
                        0 * 16, 180 * 16);
        painter.setPen(Qt::NoPen);
        break;

    case PetState::Failure:
        // 难过：眼睛画成向下的弧
        painter.setPen(QPen(faceColor, qMax(1.5, size * 0.022),
                            Qt::SolidLine, Qt::RoundCap));
        painter.drawArc(QRectF(leftEyeX - eyeRadius, eyeY - eyeRadius,
                               eyeRadius * 2, eyeRadius * 2),
                        180 * 16, 180 * 16);
        painter.drawArc(QRectF(rightEyeX - eyeRadius, eyeY - eyeRadius,
                               eyeRadius * 2, eyeRadius * 2),
                        180 * 16, 180 * 16);
        painter.setPen(Qt::NoPen);
        break;

    case PetState::Listening:
        // 监听：眼睛稍大，并在右侧画一个声音波纹
        drawEye(leftEyeX, eyeY, eyeRadius * 1.15);
        drawEye(rightEyeX, eyeY, eyeRadius * 1.15);

        painter.setPen(QPen(bodyColor.darker(160), qMax(1.5, size * 0.018)));
        painter.setBrush(Qt::NoBrush);
        for (int ring = 1; ring <= 2; ++ring) {
            const double r = size * (0.16 + 0.06 * ring);
            // 只画右半边的弧，像声波从嘴边扩散
            painter.drawArc(QRectF(bodyRect.center().x() + size * 0.16 - r,
                                  bodyRect.center().y() - r,
                                  r * 2, r * 2),
                            -60 * 16, 120 * 16);
        }
        painter.setPen(Qt::NoPen);
        break;
    }

    // ---- 嘴巴 ----
    painter.setPen(QPen(faceColor, qMax(1.5, size * 0.02),
                        Qt::SolidLine, Qt::RoundCap));
    painter.setBrush(Qt::NoBrush);

    const double mouthY = bodyRect.center().y() + size * 0.11;
    const double mouthWidth = size * 0.16;

    switch (state) {
    case PetState::Talking: {
        // 张嘴幅度随帧变化，形成说话效果
        const double openness = 0.35 + 0.65 * ((frameIndex % 2) == 0 ? 1.0 : 0.4);
        painter.setBrush(faceColor);
        painter.setPen(Qt::NoPen);
        painter.drawEllipse(QPointF(bodyRect.center().x(), mouthY),
                           mouthWidth * 0.45, mouthWidth * 0.55 * openness);
        painter.setPen(Qt::NoPen);
        break;
    }
    case PetState::Success:
        // 大笑的弧
        painter.drawArc(QRectF(bodyRect.center().x() - mouthWidth,
                              mouthY - mouthWidth * 0.6,
                              mouthWidth * 2, mouthWidth * 1.2),
                        200 * 16, 140 * 16);
        break;

    case PetState::Failure:
        // 撇嘴（向下弧）
        painter.drawArc(QRectF(bodyRect.center().x() - mouthWidth,
                              mouthY - mouthWidth * 0.3,
                              mouthWidth * 2, mouthWidth * 1.0),
                        20 * 16, 140 * 16);
        break;

    default:
        // 平静的一条小弧
        painter.drawArc(QRectF(bodyRect.center().x() - mouthWidth * 0.6,
                              mouthY - mouthWidth * 0.4,
                              mouthWidth * 1.2, mouthWidth * 0.8),
                        200 * 16, 140 * 16);
        break;
    }

    painter.end();
    return canvas;
}

} // namespace dc
