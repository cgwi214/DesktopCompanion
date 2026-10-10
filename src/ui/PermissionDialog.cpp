// ============================================================================
//  PermissionDialog.cpp — 权限确认 / 授权对话框实现
// ============================================================================
#include "ui/PermissionDialog.h"

#include "core/ErrorCode.h"
#include "core/Logger.h"
#include "ui/I18n.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFileInfo>
#include <QFontDatabase>
#include <QFrame>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QShowEvent>
#include <QVBoxLayout>

namespace dc {

namespace {

// 风险等级的视觉呈现：颜色 + 文案 key
struct RiskVisual {
    QString color;
    QString fill;
    const char *key;
};

RiskVisual riskVisual(RiskLevel level)
{
    switch (level) {
    case RiskLevel::Low:
        return { QStringLiteral("#2E7D32"), QStringLiteral("#E8F5E9"),
                 "perm.risk.low" };
    case RiskLevel::Medium:
        return { QStringLiteral("#B26A00"), QStringLiteral("#FFF4E5"),
                 "perm.risk.medium" };
    case RiskLevel::High:
        return { QStringLiteral("#C62828"), QStringLiteral("#FDECEA"),
                 "perm.risk.high" };
    case RiskLevel::Forbidden:
        return { QStringLiteral("#6A1B9A"), QStringLiteral("#F3E5F5"),
                 "perm.risk.forbidden" };
    }
    return { QStringLiteral("#555555"), QStringLiteral("#EEEEEE"),
             "perm.risk.unknown" };
}

QString sensitivityKey(SensitivityLevel level)
{
    switch (level) {
    case SensitivityLevel::Normal:    return QStringLiteral("perm.sens.normal");
    case SensitivityLevel::Sensitive: return QStringLiteral("perm.sens.sensitive");
    case SensitivityLevel::Critical:  return QStringLiteral("perm.sens.critical");
    }
    return QStringLiteral("perm.sens.normal");
}

constexpr int kValueWrapWidth = 460;

} // namespace

// ---------------------------------------------------------------------------
// 构造
// ---------------------------------------------------------------------------
PermissionDialog::PermissionDialog(const PermissionRequest &request,
                                   const PermissionPolicy &policy,
                                   QWidget *parent)
    : QDialog(parent)
    , m_request(request)
    , m_policy(policy)
{
    // SRS 4.4：授权必须由用户主动完成，不能被其它窗口遮挡或抢焦点忽略掉。
    setWindowTitle(I18n::instance().text("perm.title"));
    setModal(true);
    setMinimumWidth(540);
    setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);

    buildUi();
}

PermissionDialog::~PermissionDialog() = default;

PermissionRequest PermissionDialog::request() const
{
    return m_request;
}

// ---------------------------------------------------------------------------
// 界面组装
// ---------------------------------------------------------------------------
void PermissionDialog::buildUi()
{
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(18, 16, 18, 14);
    root->setSpacing(10);

    // ---- 顶部：动作摘要 + 风险徽章 ----
    auto *headerRow = new QHBoxLayout;
    headerRow->setSpacing(10);

    m_summaryLabel = new QLabel(buildSummaryText(), this);
    m_summaryLabel->setWordWrap(true);
    QFont summaryFont = m_summaryLabel->font();
    summaryFont.setPointSizeF(summaryFont.pointSizeF() + 1.5);
    summaryFont.setBold(true);
    m_summaryLabel->setFont(summaryFont);
    headerRow->addWidget(m_summaryLabel, 1);

    const RiskVisual rv = riskVisual(m_request.riskLevel);
    m_riskBadge = new QLabel(I18n::instance().text(rv.key), this);
    m_riskBadge->setStyleSheet(QStringLiteral(
        "color:%1; background:%2; border:1px solid %1;"
        "border-radius:9px; padding:3px 10px; font-weight:600;")
        .arg(rv.color, rv.fill));
    m_riskBadge->setAlignment(Qt::AlignCenter);
    headerRow->addWidget(m_riskBadge, 0, Qt::AlignTop);

    root->addLayout(headerRow);

    // ---- 分隔线 ----
    auto *separator = new QFrame(this);
    separator->setFrameShape(QFrame::HLine);
    separator->setStyleSheet(QStringLiteral("color:#D8DEE6;"));
    root->addWidget(separator);

    // ---- 明细区 ----
    auto *detailLayout = new QVBoxLayout;
    detailLayout->setSpacing(8);

    // 操作目标
    addInfoRow(detailLayout, QStringLiteral("perm.field.target"),
               buildTargetText(), false, true);

    // 影响范围
    addInfoRow(detailLayout, QStringLiteral("perm.field.impact"),
               buildImpactText());

    // 所需权限
    addInfoRow(detailLayout, QStringLiteral("perm.field.privilege"),
               buildPrivilegeText(), true);

    // 敏感等级（只有非普通时才单独提示，避免普通操作也刷存在感）
    if (m_request.sensitivity != SensitivityLevel::Normal) {
        addInfoRow(detailLayout, QStringLiteral("perm.field.sensitivity"),
                   I18n::instance().text(sensitivityKey(m_request.sensitivity)),
                   true);
    }

    // 涉及的具体路径
    if (!m_request.affectedPaths.isEmpty()) {
        QStringList shown;
        const int maxPaths = 10;
        for (int i = 0; i < m_request.affectedPaths.size(); ++i) {
            if (i >= maxPaths) {
                shown << I18n::instance().text(
                    "perm.paths.more",
                    QStringLiteral("count"),
                    QString::number(m_request.affectedPaths.size() - maxPaths));
                break;
            }
            shown << m_request.affectedPaths.at(i);
        }
        addInfoRow(detailLayout, QStringLiteral("perm.field.paths"),
                   shown.join(QLatin1Char('\n')), false, true);
    }

    // 涉及的注册表键 / 服务名等
    if (!m_request.affectedTargets.isEmpty()) {
        addInfoRow(detailLayout, QStringLiteral("perm.field.targets"),
                   m_request.affectedTargets.join(QLatin1Char('\n')), false, true);
    }

    root->addLayout(detailLayout);

    // ---- 管理员权限提示（SRS 4.4：不暗中提升，只说明）----
    if (m_request.elevationRequired) {
        m_elevationNotice = new QLabel(
            I18n::instance().text("perm.elevation.notice"), this);
        m_elevationNotice->setWordWrap(true);
        m_elevationNotice->setStyleSheet(QStringLiteral(
            "background:#FFF8E1; border:1px solid #F0C36D;"
            "border-radius:6px; padding:8px 10px; color:#7A5A00;"));
        root->addWidget(m_elevationNotice);
    }

    // ---- 不可逆操作的额外确认（唯一刻意的摩擦）----
    if (requiresIrreversibleAck()) {
        m_irreversibleCheck = new QCheckBox(
            I18n::instance().text("perm.ack.irreversible"), this);
        m_irreversibleCheck->setStyleSheet(
            QStringLiteral("color:#C62828; font-weight:600;"));
        connect(m_irreversibleCheck, &QCheckBox::toggled,
                this, &PermissionDialog::refreshAllowButtonState);
        root->addWidget(m_irreversibleCheck);
    }

    // ---- 批量操作提示（SRS 4.3 要求单独确认）----
    if (m_request.batchOperation) {
        auto *batchNotice = new QLabel(
            I18n::instance().text("perm.ack.batch"), this);
        batchNotice->setWordWrap(true);
        batchNotice->setStyleSheet(QStringLiteral("color:#7A5A00;"));
        root->addWidget(batchNotice);
    }

    // ---- 授权范围下拉 ----
    auto *scopeRow = new QHBoxLayout;
    scopeRow->setSpacing(8);

    auto *scopeLabel = new QLabel(
        I18n::instance().text("perm.scope.label"), this);
    scopeRow->addWidget(scopeLabel, 0);

    m_scopeCombo = new QComboBox(this);
    // 按「最小授权 -> 最大授权」的顺序加入，默认选第一项。
    //  顺序本身就是一种引导：用户顺手选到的永远是范围最小的那个。
    m_scopeCombo->addItem(I18n::instance().text("perm.scope.once"),
                          QVariant::fromValue(static_cast<int>(GrantScope::ThisActionOnly)));
    if (isScopeOfferable(GrantScope::RuleForSession)) {
        m_scopeCombo->addItem(
            I18n::instance().text("perm.scope.session"),
            QVariant::fromValue(static_cast<int>(GrantScope::RuleForSession)));
    }
    if (isScopeOfferable(GrantScope::RulePermanent)) {
        m_scopeCombo->addItem(
            I18n::instance().text("perm.scope.permanent"),
            QVariant::fromValue(static_cast<int>(GrantScope::RulePermanent)));
    }
    m_scopeCombo->setCurrentIndex(0);
    scopeRow->addWidget(m_scopeCombo, 1);

    root->addLayout(scopeRow);

    // ---- 按钮区 ----
    auto *buttonRow = new QHBoxLayout;
    buttonRow->addStretch(1);

    m_denyButton = new QPushButton(
        I18n::instance().text("perm.button.deny"), this);
    m_denyButton->setMinimumWidth(96);
    m_denyButton->setMinimumHeight(32);
    // 拒绝按钮获得焦点：按空格/回车默认就是拒绝。
    m_denyButton->setDefault(true);
    m_denyButton->setAutoDefault(true);
    connect(m_denyButton, &QPushButton::clicked, this, [this]() {
        DC_INFO(LogCat::Permission,
                QStringLiteral("User denied permission for rule '%1'")
                    .arg(m_request.ruleId));
        emit decisionMade(false, GrantScope::ThisActionOnly);
        reject();          // QDialog::reject -> exec() 返回 Rejected
    });
    buttonRow->addWidget(m_denyButton);

    m_allowButton = new QPushButton(
        I18n::instance().text("perm.button.allow"), this);
    m_allowButton->setMinimumWidth(96);
    m_allowButton->setMinimumHeight(32);
    m_allowButton->setAutoDefault(false);
    m_allowButton->setDefault(false);
    connect(m_allowButton, &QPushButton::clicked,
            this, &PermissionDialog::onAllowClicked);
    buttonRow->addWidget(m_allowButton);

    root->addLayout(buttonRow);

    // 默认按钮必须是「拒绝」—— 窗口显示后不要再改。
    m_denyButton->setFocus(Qt::OtherFocusReason);

    refreshAllowButtonState();
}

// ---------------------------------------------------------------------------
// 信息行
// ---------------------------------------------------------------------------
void PermissionDialog::addInfoRow(QVBoxLayout *layout,
                                  const QString &labelKey,
                                  const QString &value,
                                  bool emphasize,
                                  bool monospace)
{
    if (value.isEmpty())
        return;

    auto *row = new QHBoxLayout;
    row->setSpacing(10);

    auto *label = new QLabel(I18n::instance().text(labelKey), this);
    label->setFixedWidth(72);
    label->setAlignment(Qt::AlignRight | Qt::AlignTop);
    label->setStyleSheet(QStringLiteral("color:#7A8593;"));
    row->addWidget(label, 0);

    auto *valueLabel = new QLabel(value, this);
    valueLabel->setWordWrap(true);
    valueLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    if (emphasize) {
        QFont f = valueLabel->font();
        f.setBold(true);
        valueLabel->setFont(f);
    }
    if (monospace) {
        // 路径 / 注册表键用等宽字体，避免 l 和 1、0 和 O 看起来一样
        QFont f = QFontDatabase::systemFont(QFontDatabase::FixedFont);
        valueLabel->setFont(f);
    }
    valueLabel->setMaximumWidth(kValueWrapWidth);
    row->addWidget(valueLabel, 1);

    layout->addLayout(row);
}

// ---------------------------------------------------------------------------
// 文案构造（与界面分离，便于单测）
// ---------------------------------------------------------------------------
QString PermissionDialog::buildTitleText() const
{
    return I18n::instance().text("perm.title");
}

QString PermissionDialog::buildSummaryText() const
{
    if (!m_request.actionSummary.isEmpty())
        return m_request.actionSummary;

    // 规则没给摘要时兜底，至少不要让用户看到一片空白
    if (!m_request.intent.isEmpty()) {
        return I18n::instance().text(
            "perm.summary.fallback",
            QStringLiteral("intent"), m_request.intent);
    }
    return I18n::instance().text("perm.summary.unknown");
}

QString PermissionDialog::buildTargetText() const
{
    if (!m_request.targetDescription.isEmpty())
        return m_request.targetDescription;

    if (!m_request.affectedPaths.isEmpty())
        return m_request.affectedPaths.first();

    if (!m_request.affectedTargets.isEmpty())
        return m_request.affectedTargets.first();

    return I18n::instance().text("perm.target.unspecified");
}

QString PermissionDialog::buildImpactText() const
{
    // 影响说明优先用规则声明的；没有则按结构化字段"拼"出一条，
    // 而不是留空 —— 留空会让用户以为"没什么影响"，那是误导。
    if (!m_request.impactDescription.isEmpty())
        return m_request.impactDescription;

    QStringList parts;

    if (m_request.irreversible)
        parts << I18n::instance().text("perm.impact.irreversible");
    if (m_request.batchOperation) {
        int count = m_request.affectedPaths.size();
        if (count <= 0)
            count = m_request.affectedTargets.size();
        parts << (count > 0
                      ? I18n::instance().text("perm.impact.batch_count",
                                              QStringLiteral("count"),
                                              QString::number(count))
                      : I18n::instance().text("perm.impact.batch"));
    }
    if (!m_request.affectedPaths.isEmpty()) {
        parts << I18n::instance().text("perm.impact.files",
                                       QStringLiteral("count"),
                                       QString::number(m_request.affectedPaths.size()));
    }

    if (parts.isEmpty())
        return I18n::instance().text("perm.impact.minimal");

    return parts.join(I18n::instance().text("perm.impact.separator"));
}

QString PermissionDialog::buildPrivilegeText() const
{
    if (!m_request.requiredPrivilege.isEmpty())
        return m_request.requiredPrivilege;

    // 没有显式声明时，按 elevationRequired 推断并翻译成用户能懂的话
    if (m_request.elevationRequired)
        return I18n::instance().text("perm.privilege.admin");

    return I18n::instance().text("perm.privilege.user");
}

// ---------------------------------------------------------------------------
// 授权范围的可提供性
//
//  SRS 4.4 第 4、5 条：记住授权只能针对具体操作类别，且不得扩大解释。
//  因此这里做两道收窄：
//    1. 高风险或不可逆操作，不提供任何"记住"选项
//    2. 涉及关键敏感位置时，不提供"永久"选项
//    3. policy 明确关闭时，一律不提供
// ---------------------------------------------------------------------------
bool PermissionDialog::isScopeOfferable(GrantScope scope) const
{
    if (scope == GrantScope::ThisActionOnly)
        return true;         // 最小授权永远可选

    // 高风险 / 不可逆 / 批量：只允许本次
    if (m_request.riskLevel == RiskLevel::High
        || m_request.riskLevel == RiskLevel::Forbidden
        || m_request.irreversible
        || m_request.batchOperation) {
        return false;
    }

    // 关键敏感位置（系统目录、账户、注册表）：只允许本次
    if (m_request.sensitivity == SensitivityLevel::Critical)
        return false;

    if (scope == GrantScope::RuleForSession) {
        return m_policy.allowPermanentGrants || !m_request.ruleId.isEmpty();
    }

    // RulePermanent
    if (!m_policy.allowPermanentGrants)
        return false;

    // 敏感位置允许"本会话"，但不允许"永久"
    if (m_request.sensitivity == SensitivityLevel::Sensitive)
        return false;

    return true;
}

bool PermissionDialog::requiresIrreversibleAck() const
{
    return m_request.irreversible && m_request.riskLevel != RiskLevel::Low;
}

// ---------------------------------------------------------------------------
// 允许按钮的可用性
// ---------------------------------------------------------------------------
void PermissionDialog::refreshAllowButtonState()
{
    if (!m_allowButton)
        return;

    bool enabled = true;
    QString reason;

    if (requiresIrreversibleAck()
        && (!m_irreversibleCheck || !m_irreversibleCheck->isChecked())) {
        enabled = false;
        reason = I18n::instance().text("perm.tooltip.need_ack");
    }

    // 禁止级操作根本不提供允许入口（SRS 4.4 表格：禁止 -> 不提供）
    if (m_request.riskLevel == RiskLevel::Forbidden) {
        enabled = false;
        reason = I18n::instance().text("perm.tooltip.forbidden");
    }

    m_allowButton->setEnabled(enabled);
    m_allowButton->setToolTip(reason.isEmpty()
                                  ? I18n::instance().text("perm.tooltip.allow")
                                  : reason);
}

GrantScope PermissionDialog::currentScopeSelection() const
{
    if (!m_scopeCombo || m_scopeCombo->currentIndex() < 0)
        return GrantScope::ThisActionOnly;

    const int raw = m_scopeCombo->currentData().toInt();
    return static_cast<GrantScope>(raw);
}

// ---------------------------------------------------------------------------
// 允许
// ---------------------------------------------------------------------------
void PermissionDialog::onAllowClicked()
{
    // 再校验一次：按钮可能被程序化 enable 过，不能只依赖界面状态
    if (requiresIrreversibleAck()
        && (!m_irreversibleCheck || !m_irreversibleCheck->isChecked())) {
        DC_WARN(LogCat::Permission,
                QStringLiteral("Allow blocked: irreversible ack missing"));
        refreshAllowButtonState();
        return;
    }

    if (m_request.riskLevel == RiskLevel::Forbidden) {
        DC_WARN(LogCat::Permission,
                QStringLiteral("Allow blocked: risk level is Forbidden"));
        return;
    }

    const GrantScope scope = currentScopeSelection();

    DC_INFO(LogCat::Permission,
            QStringLiteral("User granted permission: rule='%1' scope='%2'")
                .arg(m_request.ruleId, grantScopeToString(scope)));

    emit decisionMade(true, scope);
    accept();              // QDialog::accept -> exec() 返回 Accepted
}

// ---------------------------------------------------------------------------
// 模态执行
// ---------------------------------------------------------------------------
PermissionDialog::Result PermissionDialog::execAndAsk(QWidget *parent)
{
    if (parent && !parentWidget())
        setParent(parent, windowFlags());

    Result result;

    const int code = QDialog::exec();

    // Accepted 只可能来自 onAllowClicked；Rejected 来自拒绝按钮 / Esc / 关闭
    result.granted = (code == QDialog::Accepted);
    result.scope = result.granted ? currentScopeSelection()
                                  : GrantScope::ThisActionOnly;
    result.userCheckedIrreversible =
        m_irreversibleCheck && m_irreversibleCheck->isChecked();

    return result;
}

// ---------------------------------------------------------------------------
// 键盘
// ---------------------------------------------------------------------------
void PermissionDialog::keyPressEvent(QKeyEvent *event)
{
    switch (event->key()) {
    case Qt::Key_Escape:
        // Esc = 拒绝。这一条很重要：用户想"什么都不做"时按 Esc，
        // 结果必须是"不授权"，绝不能是"默认同意"。
        DC_INFO(LogCat::Permission,
                QStringLiteral("Permission dialog dismissed with Esc -> denied"));
        emit decisionMade(false, GrantScope::ThisActionOnly);
        reject();
        return;

    case Qt::Key_Return:
    case Qt::Key_Enter:
        // 焦点在"拒绝"按钮上时，回车走拒绝（默认按钮行为）；
        // 焦点在"允许"按钮上时才允许。若焦点在别处，这里不代用户决定。
        if (m_allowButton && m_allowButton->hasFocus()) {
            onAllowClicked();
            return;
        }
        if (m_denyButton && m_denyButton->hasFocus()) {
            emit decisionMade(false, GrantScope::ThisActionOnly);
            reject();
            return;
        }
        return;

    default:
        break;
    }

    QDialog::keyPressEvent(event);
}

void PermissionDialog::showEvent(QShowEvent *event)
{
    QDialog::showEvent(event);

    // 每次显示都把焦点给"拒绝"。
    //  窗口管理器有可能改变焦点（例如置顶策略），这里显式再确认一次。
    if (m_denyButton)
        m_denyButton->setFocus(Qt::OtherFocusReason);
}

} // namespace dc
