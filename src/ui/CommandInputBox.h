// ============================================================================
//  CommandInputBox.h — 命令输入框
//
//  需求依据：SRS V1.1 4.2
//    TXT-003：提供快捷输入方式，可通过快捷键唤出
//    TXT-004：输入框可输入自然语言命令
//    TXT-001：支持中文、英语、日语输入
//
//  关键设计决定：
//
//    1. 【这是一个"发送意图"的框，不是"执行命令"的框】
//       输入框只做两件事：收集文本、把文本交给 IntentRouter。
//       它不知道"打开浏览器"会走到 SystemService，
//       也不知道"翻译"要联网 —— 这些全是路由层的事。
//       这直接落实 SRS 3.2 的 UI 层禁令。
//
//    2. 【必须可以被 Esc 关掉，且不留下副作用】
//       SRS 4.12 规定 Esc 关闭当前气泡、弹窗或取消当前输入。
//       取消就是取消：不产生历史记录，不发请求。
//
//    3. 【历史回看用本地列表，不查数据库】
//       输入框里的"上一次输入"是操作便利，不是历史记录功能。
//       查数据库会让按键有延迟（SRS 7.1 要求响应及时），
//       而且会牵进隐私设置判断（历史关掉了就不该看到）。
//       所以这里只保留本次会话内的输入历史。
//
//    4. 【输入长度在本地先截断】
//       SRS 7.3 要求限制输入长度防滥用。在输入框就限制的好处是
//       用户立刻看到（字数提示），而不是提交后才被告知失败。
//
//    5. 【语音结果也走同一个入口】
//       SRS 4.5 SPK-006：语音识别结果必须进入 CommandEngine。
//       所以输入框有 setTextAndSubmit() —— 语音模块识别完成后
//       把文本塞进来，走完全一样的链路。
//       这样"语音可以做的事"和"打字可以做的事"天然一致，
//       不会出现某类命令只有语音能用的情况。
//
//  视觉：无边框圆角面板，居中显示在桌宠上方或屏幕中央。
// ============================================================================
#pragma once

#include "ui/I18n.h"

#include <QPoint>
#include <QString>
#include <QStringList>
#include <QWidget>

QT_BEGIN_NAMESPACE
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
QT_END_NAMESPACE

namespace dc {

// ---------------------------------------------------------------------------
// 输入框配置
// ---------------------------------------------------------------------------
struct CommandInputConfig {
    int maxInputLength = 512;      // 超过则拒绝提交（SRS 7.3）
    int maxSessionHistory = 20;    // 会话内回看条数
    int width = 520;
    int minHeight = 64;
    QString placeholder;
    bool showCharCounter = true;
};

// ---------------------------------------------------------------------------
// CommandInputBox
// ---------------------------------------------------------------------------
class CommandInputBox : public QWidget
{
    Q_OBJECT

public:
    explicit CommandInputBox(QWidget *parent = nullptr);
    ~CommandInputBox() override;

    void setConfig(const CommandInputConfig &config);
    CommandInputConfig config() const;

    // -----------------------------------------------------------------------
    // 唤出 / 收起
    //
    //  anchor 非空时显示在锚点上方（贴着桌宠），否则屏幕中央。
    //  每次唤出都会清空上一次的输入 —— 让"打开输入框"永远是一个
    //  干净的状态，用户不用先删掉旧内容。
    // -----------------------------------------------------------------------
    void summon(const QRect &anchor = QRect());
    void dismiss();

    bool isSummoned() const;

    // -----------------------------------------------------------------------
    // 塞入文本并立即提交（语音模块用，SRS 4.5 SPK-006）
    //
    //  会先唤出再提交。"立即提交"是刻意的：用户说完话之后
    //  不应该再需要按一次回车。
    // -----------------------------------------------------------------------
    void setTextAndSubmit(const QString &text);

    // 当前输入内容（供测试）
    QString currentText() const;

    // 把一条输入加入回看历史（提交成功后由界面调用）
    void rememberInput(const QString &text);
    QStringList sessionHistory() const;

    // -----------------------------------------------------------------------
    // 提示区：显示一次性的错误/说明（例如"输入过长"）
    //   hint 为空时隐藏提示区
    // -----------------------------------------------------------------------
    void showHint(const QString &hint, bool isError = false);
    void clearHint();

signals:
    // 用户提交了一条命令（这是本类唯一的对外输出）
    //   注意信号名不叫 "execute" —— 它只是"请求"，执行是路由层的事。
    void submitted(const QString &text);

    // 用户取消了输入（Esc 或点了关闭）
    void cancelled();

    // 请求切换成"语音输入"（界面据此通知语音模块开始识别）
    void voiceInputRequested();

protected:
    void paintEvent(QPaintEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void focusOutEvent(QFocusEvent *event) override;

private:
    // 触发一次提交（含长度校验）
    void trySubmit();

    // 把历史里上一条/下一条填进输入框（上下方向键）
    void navigateHistory(int delta);

    // 刷新字数提示与提交按钮可用性
    void refreshCounter();

    // 根据 config 与内容调整尺寸并居中/贴靠
    void relayout(const QRect &anchor);

    // 计算应显示的位置
    QPoint computePosition(const QRect &anchor, const QSize &size) const;

    CommandInputConfig m_config;

    QLineEdit *m_input = nullptr;
    QLabel *m_hintLabel = nullptr;
    QLabel *m_counterLabel = nullptr;
    QPushButton *m_submitButton = nullptr;
    QPushButton *m_voiceButton = nullptr;
    QListWidget *m_suggestionList = nullptr;

    // 会话内输入历史（新的在前）
    QStringList m_history;
    // 当前浏览到历史里的第几条；-1 表示不在浏览状态
    int m_historyCursor = -1;
    // 浏览历史前，用户自己打的内容（浏览时先存起来，回到尽头要还给他）
    QString m_draftBeforeHistory;

    bool m_summoned = false;

    // 无边框拖动
    bool m_dragging = false;
    QPoint m_dragOffset;

    QRect m_anchor;
};

} // namespace dc
