// ============================================================================
//  PermissionDialog.h — 权限确认 / 授权对话框
//
//  需求依据：SRS V1.1 第 4.4 节（系统操作与权限）
//    - 需高权限的操作应明确告知用户，并由用户主动确认
//    - 展示操作目标、影响范围和所需权限
//    - 「记住授权」只能针对具体操作类别，不得扩大解释一次授权
//    - 用户取消或拒绝后，不得通过其他路径重试同一高风险操作
//    - 默认以普通用户权限运行，不得暗中取得管理员权限
//
//  这个对话框是整套安全性设计的**人机界面**。它做的事情很少，
//  但做错的代价很大，所以这里把每个视觉/交互决定都写清楚原因：
//
//    1. 【默认按钮是「拒绝」，不是「允许」】
//       用户按回车时应该发生的是"什么都不做"。
//       把「允许」设成默认，等于让手快的人被动授权高风险操作。
//
//    2. 【必须点中按钮，不能整窗点击确认】
//       防止"点哪都算同意"的误触。目标/影响文本区域全部不可点选确认。
//
//    3. 【不可逆操作要额外打勾】
//       删除、覆盖这类操作，用户在勾选"我明白此操作不可撤销"之前
//       「允许」按钮保持禁用。这是唯一一处刻意的摩擦，因为它值得。
//
//    4. 【「记住授权」是下拉选择，不是复选框】
//       复选框只有"记住/不记住"，会诱导用户点成"永久记住"。
//       下拉里三个选项按最小授权排序，默认落在"仅本次"。
//       并且当风险为高、或涉及敏感位置时，直接隐藏长期选项 ——
//       不提供"不该给的选择"，比提供了再劝阻更可靠。
//
//    5. 【管理员权限没有"自动提升"选项】
//       SRS 明确禁止暗中取得管理员权限。所以当 elevationRequired
//       为真时，界面只说明"需要管理员权限"，并告诉用户
//       系统可能会弹出安全提示，由用户决定 —— 本程序不代劳。
//
//    6. 【显示的是脱敏后的路径吗？】
//       不。这里必须显示完整路径。脱敏是给"日志"用的（SRS 7.3），
//       不是给"授权决策"用的：用户有权知道到底要动哪个文件，
//       否则这个确认框就是走过场。
// ============================================================================
#pragma once

#include "engine/PermissionManager.h"
#include "model/PermissionTypes.h"

#include <QDialog>
#include <QString>

QT_BEGIN_NAMESPACE
class QCheckBox;
class QComboBox;
class QLabel;
class QPushButton;
class QVBoxLayout;
QT_END_NAMESPACE

namespace dc {

// ---------------------------------------------------------------------------
// PermissionDialog
// ---------------------------------------------------------------------------
class PermissionDialog : public QDialog
{
    Q_OBJECT

public:
    // -----------------------------------------------------------------------
    // 用法（同步阻塞）：
    //
    //   PermissionDialog dlg(request, policy);
    //   const PermissionDialogResult r = dlg.execAndAsk(parentWidget);
    //   if (r.accepted()) { router->resolveAuthorization(true, r.scope); }
    //   else             { router->resolveAuthorization(false, ...); }
    //
    //  这里不把 IntentRouter 直接连进来，是因为对话框不该知道
    //  "授权之后会发生什么"。它只回答"用户同不同意"。
    // -----------------------------------------------------------------------
    struct Result {
        bool granted = false;
        GrantScope scope = GrantScope::ThisActionOnly;
        bool userCheckedIrreversible = false;

        bool accepted() const { return granted; }
    };

    explicit PermissionDialog(const PermissionRequest &request,
                              const PermissionPolicy &policy = PermissionPolicy(),
                              QWidget *parent = nullptr);
    ~PermissionDialog() override;

    // 模态弹出，阻塞直到用户作出选择
    Result execAndAsk(QWidget *parent = nullptr);

    // 当前请求（供测试）
    PermissionRequest request() const;

    // 构造文案（公开以便单元测试直接核对措辞，不需要真的弹窗）
    QString buildTitleText() const;
    QString buildSummaryText() const;
    QString buildTargetText() const;
    QString buildImpactText() const;
    QString buildPrivilegeText() const;

    // 是否应该显示某个授权范围选项（公开以便单测）
    bool isScopeOfferable(GrantScope scope) const;

    // 是否要求用户勾选"不可撤销"
    bool requiresIrreversibleAck() const;

signals:
    // 用户做出了决定（无论同意还是拒绝都会发）
    void decisionMade(bool granted, dc::GrantScope scope);

protected:
    void keyPressEvent(QKeyEvent *event) override;
    void showEvent(QShowEvent *event) override;

private:
    // 组装界面
    void buildUi();

    // 加一行"标签 + 值"的信息
    void addInfoRow(QVBoxLayout *layout,
                    const QString &labelKey,
                    const QString &value,
                    bool emphasize = false,
                    bool monospace = false);

    // 根据当前勾选/选择状态刷新「允许」按钮可用性
    void refreshAllowButtonState();

    // 用户点了允许
    void onAllowClicked();

    // 从下拉里读出当前选择的授权范围
    GrantScope currentScopeSelection() const;

    PermissionRequest m_request;
    PermissionPolicy m_policy;

    QLabel *m_riskBadge = nullptr;
    QLabel *m_summaryLabel = nullptr;
    QLabel *m_targetLabel = nullptr;
    QLabel *m_impactLabel = nullptr;
    QLabel *m_privilegeLabel = nullptr;
    QLabel *m_elevationNotice = nullptr;
    QLabel *m_pathsLabel = nullptr;
    QLabel *m_targetsLabel = nullptr;

    QCheckBox *m_irreversibleCheck = nullptr;
    QComboBox *m_scopeCombo = nullptr;

    QPushButton *m_allowButton = nullptr;
    QPushButton *m_denyButton = nullptr;
};

} // namespace dc
