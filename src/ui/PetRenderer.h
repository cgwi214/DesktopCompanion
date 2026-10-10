// ============================================================================
//  PetRenderer.h — 桌宠形象渲染
//
//  需求依据：SRS V1.1 4.1
//    PET-001：支持静态图片（PNG / JPG / GIF 首帧）
//    PET-002：支持序列帧动画
//    PET-003：支持透明背景与无边框窗口
//    PET-004：支持缩放
//    PET-006：用户自定义静态图片时只提供有限反馈
//    PET-007：形象资源缺失时使用内置占位形象（不得显示破图）
//
//  设计说明（为什么渲染独立于窗口）：
//    PetWindow 管窗口行为（无边框、置顶、拖动、托盘），
//    PetRenderer 管"一张位图长什么样"。
//    分开以后有三点好处：
//      1. 缩放、缓存、缺图回退这些逻辑可以单元测试（不需要真窗口）
//      2. 以后要换成 QOpenGLWidget 或 QPainter 以外的后端只改这一处
//      3. 窗口代码短，容易检查有没有违反"不阻塞 UI"
//
//  资源约定（resources/pets/<skinId>/）：
//      <skinId>/idle/000.png, 001.png, ...
//      <skinId>/talking/000.png, ...
//      <skinId>/working/...
//      <skinId>/success/...
//      <skinId>/failure/...
//      <skinId>/listening/...
//      <skinId>/skin.json        —— 可选，声明每组的帧数与帧间隔
//
//  找不到任何图片时的行为（PET-007）：
//    用 QPainter 画一个内置的简单形象（一个带表情的圆）。
//    绝不显示"图片加载失败"的破图图标。
// ============================================================================
#pragma once

#include "model/Types.h"
#include "ui/PetStateMachine.h"

#include <QHash>
#include <QObject>
#include <QPixmap>
#include <QSize>
#include <QString>
#include <QStringList>

namespace dc {

// ---------------------------------------------------------------------------
// 渲染资源来源
// ---------------------------------------------------------------------------
enum class PetAssetKind {
    BuiltinPlaceholder = 0,   // 内置占位形象（代码绘制）
    StaticImage,              // 用户指定的单张静态图片
    FrameSequence             // 序列帧动画目录
};

QString petAssetKindToString(PetAssetKind kind);

// ---------------------------------------------------------------------------
// 渲染配置（由 AppConfig::ui 映射而来）
// ---------------------------------------------------------------------------
struct PetRenderConfig {
    PetAssetKind kind = PetAssetKind::BuiltinPlaceholder;

    QString skinId = QStringLiteral("default");   // 内置皮肤 id
    QString skinsRoot;                            // 皮肤根目录
    QString staticImagePath;                      // StaticImage 时的图片路径
    QString animationDir;                         // FrameSequence 时的目录

    double scale = 1.0;              // 0.5 ~ 3.0
    int frameIntervalOverrideMs = 0; // >0 时覆盖 skin.json 里的帧间隔

    bool smoothScale = true;         // 缩放是否平滑（像素风应关掉）
    Qt::TransformationMode transformMode = Qt::SmoothTransformation;
};

// ---------------------------------------------------------------------------
// 一张待绘制的帧
// ---------------------------------------------------------------------------
struct PetFrame {
    QPixmap pixmap;        // 已按 scale 缩放并缓存
    bool valid = false;    // false 表示要用内置占位形象
    QSize logicalSize;     // 未缩放的原始尺寸（用于窗口大小计算）

    bool isValid() const { return valid && !pixmap.isNull(); }
};

// ---------------------------------------------------------------------------
// PetRenderer
// ---------------------------------------------------------------------------
class PetRenderer : public QObject
{
    Q_OBJECT

public:
    explicit PetRenderer(QObject *parent = nullptr);
    ~PetRenderer() override;

    // -----------------------------------------------------------------------
    // 初始化
    //
    //  扫描并加载全部状态所需的帧。
    //  返回成功加载的状态组数量（0 表示将使用内置占位形象，
    //  这不是错误，但界面应该告知用户"没有找到形象资源"）。
    // -----------------------------------------------------------------------
    int setConfig(const PetRenderConfig &config);
    PetRenderConfig config() const;

    // -----------------------------------------------------------------------
    // 取帧
    //
    //  state + frameIndex -> 可直接 drawPixmap 的 QPixmap。
    //  任何取不到的情况都返回"内置占位形象"对应的帧，
    //  因此调用方**不需要**判空（PET-007）。
    // -----------------------------------------------------------------------
    PetFrame frame(PetState state, int frameIndex) const;

    // 当前状态下总共几帧（用于窗口尺寸与状态机配置）
    int frameCount(PetState state) const;

    // 该状态的帧间隔（毫秒）
    int frameInterval(PetState state) const;

    // 是否正在使用内置占位形象
    bool usingPlaceholder() const;

    // 已加载成功的状态组名（供"资源状态"诊断展示）
    QStringList loadedStates() const;

    // 加载过程中遇到的问题（面向用户，例如"某张图损坏已跳过"）
    QStringList loadWarnings() const;

    // -----------------------------------------------------------------------
    // 窗口尺寸
    //
    //  所有状态里最大的那一帧的显示尺寸。
    //  窗口按这个尺寸固定，状态切换时不会跳来跳去 —— 视觉上更稳。
    // -----------------------------------------------------------------------
    QSize displaySize() const;

    // 重新按当前 scale 生成缓存（用户改缩放后调用）
    void rebuildCache();

    // -----------------------------------------------------------------------
    // 供状态机使用：把渲染资源的信息填进状态机配置
    //
    //  这样"有几帧、多快"只有一个来源（资源本身），
    //  不会出现状态机以为有 4 帧而实际只有 3 张图的情况。
    // -----------------------------------------------------------------------
    PetStateMachineConfig toStateMachineConfig() const;

    // -----------------------------------------------------------------------
    // 内置占位形象的绘制
    //
    //  公开以便单元测试直接调用（不需要真实文件系统）。
    //  state 影响表情：Idle 微笑、Working 专注、Success 开心、Failure 难过。
    // -----------------------------------------------------------------------
    static QPixmap drawPlaceholder(PetState state, int frameIndex, int sizePx);

signals:
    // 加载完成（界面据此重新计算窗口尺寸）
    void assetsReloaded();

private:
    // 从目录加载一个状态的序列帧
    bool loadStateFrames(PetState state, const QString &directory);

    // 加载单张静态图片，并复制到全部状态（静态图只有一个表情）
    bool loadStaticImage(const QString &path);

    // 读取 skin.json（可选文件）
    bool loadSkinManifest(const QString &directory);

    // 缩放并缓存一张原始图
    QPixmap scaled(const QPixmap &source) const;

    // 缩放后的目标尺寸
    QSize scaledSize(const QSize &original) const;

    // 该状态的动画组名 -> 目录名
    static QString stateDirectoryName(PetState state);

    PetRenderConfig m_config;

    // 状态 -> 帧列表（已缩放）
    QHash<int, QList<QPixmap>> m_frames;         // key = static_cast<int>(PetState)
    // 状态 -> 原始尺寸
    QHash<int, QSize> m_originalSizes;
    // 状态 -> 帧间隔（毫秒）
    QHash<int, int> m_frameIntervals;

    QSize m_displaySize;
    bool m_usingPlaceholder = true;
    QStringList m_loadedStates;
    QStringList m_loadWarnings;
};

} // namespace dc

Q_DECLARE_METATYPE(dc::PetAssetKind)
Q_DECLARE_METATYPE(dc::PetRenderConfig)
Q_DECLARE_METATYPE(dc::PetFrame)
