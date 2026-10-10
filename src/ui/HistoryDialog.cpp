// ============================================================================
//  HistoryDialog.cpp — 历史记录窗口实现
// ============================================================================
#include "ui/HistoryDialog.h"

#include "core/Logger.h"
#include "services/HistoryService.h"
#include "ui/I18n.h"

#include <QApplication>
#include <QClipboard>
#include <QCloseEvent>
#include <QComboBox>
#include <QDateTimeEdit>
#include <QDir>
#include <QFileDialog>
#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QJsonDocument>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QTabWidget>
#include <QTableWidget>
#include <QVBoxLayout>

namespace dc {

namespace {

// 表格里存记录 id 的角色
constexpr int kIdRole = Qt::UserRole + 1;

// 时间列显示格式
const char *kTimeFormat = "yyyy-MM-dd HH:mm:ss";

// 列表加载上限（防止一次拉十万条把界面卡住，SRS 7.1）
constexpr int kPageSize = 200;

} // namespace

// ---------------------------------------------------------------------------
// 构造
// ---------------------------------------------------------------------------
HistoryDialog::HistoryDialog(HistoryService *history, QWidget *parent)
    : QDialog(parent)
    , m_history(history)
{
    setWindowTitle(I18n::instance().text("history.title"));
    setMinimumSize(820, 560);
    setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);

    buildUi();
    buildTabs();
    updateStats();

    if (m_history) {
        connect(m_history, &HistoryService::historyChanged,
                this, [this](dc::HistoryCategory) {
                    // 服务报告数据变了（可能是别处触发的删除），重载当前页。
                    //  不做全量刷新：别的标签页等用户切过去时自然会刷新。
                    populateCurrentTab();
                    updateStats();
                });
        connect(m_history, &HistoryService::storageWarning,
                this, [this](const QString &message) {
                    if (m_statsLabel) {
                        m_statsLabel->setStyleSheet(
                            QStringLiteral("color:#C62828;"));
                        m_statsLabel->setText(message);
                    }
                });
    }
}

HistoryDialog::~HistoryDialog() = default;

// ---------------------------------------------------------------------------
// 界面
// ---------------------------------------------------------------------------
void HistoryDialog::buildUi()
{
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(14, 12, 14, 12);
    root->setSpacing(9);

    // ---- 检索行 ----
    auto *searchRow = new QHBoxLayout;
    searchRow->setSpacing(8);

    m_searchInput = new QLineEdit(this);
    m_searchInput->setPlaceholderText(
        I18n::instance().text("history.search.placeholder"));
    m_searchInput->setClearButtonEnabled(true);
    connect(m_searchInput, &QLineEdit::returnPressed,
            this, &HistoryDialog::onSearchTriggered);
    searchRow->addWidget(m_searchInput, 1);

    m_fromEdit = new QDateTimeEdit(this);
    m_fromEdit->setCalendarPopup(true);
    m_fromEdit->setDisplayFormat(QString::fromLatin1(kTimeFormat));
    // 默认不启用时间范围：勾选框般的语义用 setSpecialValueText 表达太绕，
    //  改用"清空 = 不限"的显式做法（见 onClearSearch）。
    m_fromEdit->setDateTime(QDateTime::currentDateTime().addMonths(-1));
    m_fromEdit->setToolTip(I18n::instance().text("history.range.from"));
    searchRow->addWidget(m_fromEdit, 0);

    m_toEdit = new QDateTimeEdit(this);
    m_toEdit->setCalendarPopup(true);
    m_toEdit->setDisplayFormat(QString::fromLatin1(kTimeFormat));
    m_toEdit->setDateTime(QDateTime::currentDateTime());
    m_toEdit->setToolTip(I18n::instance().text("history.range.to"));
    searchRow->addWidget(m_toEdit, 0);

    m_searchButton = new QPushButton(
        I18n::instance().text("history.search.button"), this);
    connect(m_searchButton, &QPushButton::clicked,
            this, &HistoryDialog::onSearchTriggered);
    searchRow->addWidget(m_searchButton);

    m_clearSearchButton = new QPushButton(
        I18n::instance().text("history.search.clear"), this);
    connect(m_clearSearchButton, &QPushButton::clicked,
            this, &HistoryDialog::onClearSearch);
    searchRow->addWidget(m_clearSearchButton);

    root->addLayout(searchRow);

    // ---- 标签页 ----
    m_tabs = new QTabWidget(this);
    connect(m_tabs, &QTabWidget::currentChanged,
            this, &HistoryDialog::onTabChanged);
    root->addWidget(m_tabs, 1);

    // ---- 隐私说明（关掉的类别不出现，这里说明当前记录范围）----
    m_privacyNoteLabel = new QLabel(this);
    m_privacyNoteLabel->setWordWrap(true);
    m_privacyNoteLabel->setStyleSheet(
        QStringLiteral("color:#7A8593; padding:2px 0;"));
    root->addWidget(m_privacyNoteLabel);

    // ---- 统计 ----
    m_statsLabel = new QLabel(this);
    m_statsLabel->setWordWrap(true);
    m_statsLabel->setStyleSheet(QStringLiteral("color:#5A6472;"));
    root->addWidget(m_statsLabel);

    auto *sep = new QFrame(this);
    sep->setFrameShape(QFrame::HLine);
    sep->setStyleSheet(QStringLiteral("color:#D8DEE6;"));
    root->addWidget(sep);

    // ---- 操作按钮 ----
    auto *buttonRow = new QHBoxLayout;
    buttonRow->setSpacing(8);

    m_deleteButton = new QPushButton(
        I18n::instance().text("history.button.delete_selected"), this);
    connect(m_deleteButton, &QPushButton::clicked,
            this, &HistoryDialog::onDeleteSelected);
    buttonRow->addWidget(m_deleteButton);

    m_deleteRangeButton = new QPushButton(
        I18n::instance().text("history.button.delete_range"), this);
    connect(m_deleteRangeButton, &QPushButton::clicked,
            this, &HistoryDialog::onDeleteByRange);
    buttonRow->addWidget(m_deleteRangeButton);

    m_clearCategoryButton = new QPushButton(
        I18n::instance().text("history.button.clear_category"), this);
    connect(m_clearCategoryButton, &QPushButton::clicked,
            this, &HistoryDialog::onClearCategory);
    buttonRow->addWidget(m_clearCategoryButton);

    m_clearAllButton = new QPushButton(
        I18n::instance().text("history.button.clear_all"), this);
    m_clearAllButton->setStyleSheet(
        QStringLiteral("color:#C62828; font-weight:600;"));
    connect(m_clearAllButton, &QPushButton::clicked,
            this, &HistoryDialog::onClearEverything);
    buttonRow->addWidget(m_clearAllButton);

    buttonRow->addStretch(1);

    m_exportButton = new QPushButton(
        I18n::instance().text("history.button.export"), this);
    connect(m_exportButton, &QPushButton::clicked,
            this, &HistoryDialog::onExport);
    buttonRow->addWidget(m_exportButton);

    m_refreshButton = new QPushButton(
        I18n::instance().text("history.button.refresh"), this);
    connect(m_refreshButton, &QPushButton::clicked, this, [this]() {
        populateCurrentTab();
        updateStats();
    });
    buttonRow->addWidget(m_refreshButton);

    m_closeButton = new QPushButton(
        I18n::instance().text("history.button.close"), this);
    connect(m_closeButton, &QPushButton::clicked, this, &QDialog::hide);
    buttonRow->addWidget(m_closeButton);

    root->addLayout(buttonRow);
}

// ---------------------------------------------------------------------------
// 标签页
//
//  只有"被启用记录"的类别才建标签页（见头文件里的说明）。
// ---------------------------------------------------------------------------
void HistoryDialog::buildTabs()
{
    if (!m_tabs)
        return;

    m_tabs->clear();
    m_tables.clear();

    // 没有任何服务或数据库不可用时，给一个明确的空状态说明
    if (!m_history || !m_history->isAvailable()) {
        auto *placeholder = new QTableWidget(0, 1, m_tabs);
        placeholder->setHorizontalHeaderLabels(
            { I18n::instance().text("history.unavailable.title") });
        m_tabs->addTab(placeholder, I18n::instance().text("history.tab.none"));

        if (m_privacyNoteLabel) {
            m_privacyNoteLabel->setText(
                I18n::instance().text("history.unavailable.detail"));
        }
        return;
    }

    const PrivacySettings privacy = m_history->privacySettings();

    struct CategorySpec {
        HistoryCategory category;
        bool enabled;
    };

    const CategorySpec specs[] = {
        { HistoryCategory::Conversation, privacy.conversationEnabled },
        { HistoryCategory::Task,         privacy.taskEnabled },
        { HistoryCategory::Translation,  privacy.translationEnabled },
        { HistoryCategory::Search,       privacy.searchEnabled },
        { HistoryCategory::Recording,    privacy.recordingEnabled },
    };

    int enabledCount = 0;
    QStringList disabledNames;

    for (const CategorySpec &spec : specs) {
        if (!spec.enabled) {
            // 记录被关闭的类别，稍后在底部说明里列出。
            //  这是"不显示但要说清楚"，而不是"悄悄消失"。
            disabledNames << categoryTitle(spec.category);
            continue;
        }

        auto *table = new QTableWidget(0, headersFor(spec.category).size(),
                                       m_tabs);
        table->setHorizontalHeaderLabels(headersFor(spec.category));
        table->setSelectionBehavior(QAbstractItemView::SelectRows);
        table->setSelectionMode(QAbstractItemView::ExtendedSelection);
        table->setEditTriggers(QAbstractItemView::NoEditTriggers);
        table->setAlternatingRowColors(true);
        table->setFrameShape(QFrame::NoFrame);
        table->verticalHeader()->setVisible(false);
        table->horizontalHeader()->setStretchLastSection(true);
        table->horizontalHeader()->setSectionResizeMode(
            0, QHeaderView::ResizeToContents);
        table->setWordWrap(false);

        // 双击复制该行主文本，方便用户把内容拿走
        connect(table, &QTableWidget::itemDoubleClicked, this,
                [](QTableWidgetItem *item) {
                    if (!item)
                        return;
                    QApplication::clipboard()->setText(item->text());
                });

        const int index = m_tabs->addTab(table, categoryTitle(spec.category));
        m_tables.insert(static_cast<int>(spec.category), table);

        // 记住每个标签页对应哪个类别（切页时要用）
        m_tabs->setTabToolTip(index, categoryTitle(spec.category));

        ++enabledCount;
    }

    if (m_privacyNoteLabel) {
        if (enabledCount == 0) {
            m_privacyNoteLabel->setText(
                I18n::instance().text("history.privacy.all_off"));
            m_privacyNoteLabel->setStyleSheet(
                QStringLiteral("color:#7A5A00; padding:2px 0;"));
        } else if (!disabledNames.isEmpty()) {
            m_privacyNoteLabel->setText(
                I18n::instance().text("history.privacy.partial_off",
                                      QStringLiteral("categories"),
                                      disabledNames.join(
                                          I18n::instance().text("history.privacy.separator"))));
            m_privacyNoteLabel->setStyleSheet(
                QStringLiteral("color:#7A8593; padding:2px 0;"));
        } else {
            m_privacyNoteLabel->setText(
                I18n::instance().text("history.privacy.all_on"));
            m_privacyNoteLabel->setStyleSheet(
                QStringLiteral("color:#7A8593; padding:2px 0;"));
        }
    }
}

QString HistoryDialog::categoryTitle(HistoryCategory category)
{
    switch (category) {
    case HistoryCategory::Conversation:
        return I18n::instance().text("history.tab.conversation");
    case HistoryCategory::Task:
        return I18n::instance().text("history.tab.task");
    case HistoryCategory::Translation:
        return I18n::instance().text("history.tab.translation");
    case HistoryCategory::Search:
        return I18n::instance().text("history.tab.search");
    case HistoryCategory::Recording:
        return I18n::instance().text("history.tab.recording");
    }
    return I18n::instance().text("history.tab.unknown");
}

QStringList HistoryDialog::headersFor(HistoryCategory category)
{
    const I18n &i18n = I18n::instance();
    switch (category) {
    case HistoryCategory::Conversation:
        return { i18n.text("history.col.time"),
                 i18n.text("history.col.role"),
                 i18n.text("history.col.content") };
    case HistoryCategory::Task:
        return { i18n.text("history.col.time"),
                 i18n.text("history.col.command"),
                 i18n.text("history.col.status"),
                 i18n.text("history.col.result") };
    case HistoryCategory::Translation:
        return { i18n.text("history.col.time"),
                 i18n.text("history.col.direction"),
                 i18n.text("history.col.source"),
                 i18n.text("history.col.translated") };
    case HistoryCategory::Search:
        return { i18n.text("history.col.time"),
                 i18n.text("history.col.query"),
                 i18n.text("history.col.provider") };
    case HistoryCategory::Recording:
        return { i18n.text("history.col.time"),
                 i18n.text("history.col.file"),
                 i18n.text("history.col.duration"),
                 i18n.text("history.col.status") };
    }
    return { i18n.text("history.col.time") };
}

// ---------------------------------------------------------------------------
// 打开
// ---------------------------------------------------------------------------
void HistoryDialog::open()
{
    // 每次打开都重建标签页：用户可能在设置里刚改了隐私开关
    buildTabs();
    populateCurrentTab();
    updateStats();

    show();
    raise();
    activateWindow();
}

void HistoryDialog::reload()
{
    populateCurrentTab();
    updateStats();
}

// ---------------------------------------------------------------------------
// 当前标签页
// ---------------------------------------------------------------------------
HistoryCategory HistoryDialog::currentCategory() const
{
    if (!m_tabs || m_tabs->currentIndex() < 0)
        return HistoryCategory::Conversation;

    QWidget *widget = m_tabs->currentWidget();
    for (auto it = m_tables.constBegin(); it != m_tables.constEnd(); ++it) {
        if (it.value() == widget)
            return static_cast<HistoryCategory>(it.key());
    }

    return HistoryCategory::Conversation;
}

HistoryQuery HistoryDialog::currentQuery() const
{
    HistoryQuery query;
    query.limit = kPageSize;

    if (m_searchInput)
        query.keyword = m_searchInput->text().trimmed();

    // 时间范围只在用户点了"检索"或明确设过时才生效。
    //  这里用 m_filtered 作为闸门：未检索状态下不按时间过滤，
    //  这样默认打开就能看到最近的记录，而不是被默认的一个月范围卡住。
    if (m_filtered) {
        if (m_fromEdit)
            query.from = m_fromEdit->dateTime();
        if (m_toEdit)
            query.to = m_toEdit->dateTime();
    }

    return query;
}

void HistoryDialog::onTabChanged(int index)
{
    Q_UNUSED(index);
    // 切页不做查询 —— 只在确实有表格的类别上加载。
    //  这样快速点几个标签页不会触发一串数据库查询。
    populateCurrentTab();
}

// ---------------------------------------------------------------------------
// 数据填充
// ---------------------------------------------------------------------------
void HistoryDialog::populateCurrentTab()
{
    m_currentRowIds.clear();

    const HistoryCategory category = currentCategory();
    QTableWidget *table = m_tables.value(static_cast<int>(category), nullptr);
    if (!table)
        return;

    if (!m_history || !m_history->isAvailable()) {
        table->setRowCount(0);
        return;
    }

    const HistoryQuery query = currentQuery();

    // 统一走 searchAll 后按类别筛？
    //  不。searchAll 是给"全部"标签页用的，它会把五张表都查一遍。
    //  这里明确知道要哪一类，就只查那一类 —— 少查四张表。
    QVector<HistoryEntry> entries;

    switch (category) {
    case HistoryCategory::Conversation: {
        const auto rows = m_history->queryConversation(query);
        for (const auto &r : rows) {
            HistoryEntry e;
            e.category = category;
            e.id = r.id;
            e.createdAt = r.createdAt;
            e.title = r.content.left(120);
            e.subtitle = r.role;
            e.payload = r.toJson();
            entries.append(e);
        }
        break;
    }
    case HistoryCategory::Task: {
        const auto rows = m_history->queryTasks(query);
        for (const auto &r : rows) {
            HistoryEntry e;
            e.category = category;
            e.id = r.id;
            e.createdAt = r.createdAt;
            e.title = r.commandId;
            e.subtitle = r.status;
            e.payload = r.toJson();
            entries.append(e);
        }
        break;
    }
    case HistoryCategory::Translation: {
        const auto rows = m_history->queryTranslations(query);
        for (const auto &r : rows) {
            HistoryEntry e;
            e.category = category;
            e.id = r.id;
            e.createdAt = r.createdAt;
            e.title = r.sourceText.left(120);
            e.subtitle = QStringLiteral("%1 → %2").arg(r.sourceLang, r.targetLang);
            e.payload = r.toJson();
            entries.append(e);
        }
        break;
    }
    case HistoryCategory::Search: {
        const auto rows = m_history->querySearches(query);
        for (const auto &r : rows) {
            HistoryEntry e;
            e.category = category;
            e.id = r.id;
            e.createdAt = r.createdAt;
            e.title = r.query;
            e.subtitle = r.providerId;
            e.payload = r.toJson();
            entries.append(e);
        }
        break;
    }
    case HistoryCategory::Recording: {
        const auto rows = m_history->queryRecordings(query);
        for (const auto &r : rows) {
            HistoryEntry e;
            e.category = category;
            e.id = r.id;
            e.createdAt = r.createdAt;
            e.title = r.filePath;
            e.subtitle = QString::number(r.duration);
            e.payload = r.toJson();
            entries.append(e);
        }
        break;
    }
    }

    table->setRowCount(entries.size());
    for (int i = 0; i < entries.size(); ++i)
        fillRow(table, i, entries.at(i));

    // 空结果给一句提示（而不是一片白）
    if (entries.isEmpty()) {
        table->setRowCount(1);
        auto *item = new QTableWidgetItem(
            m_filtered ? I18n::instance().text("history.empty.no_match")
                       : I18n::instance().text("history.empty.none"));
        item->setForeground(QColor(0x9A, 0xA4, 0xB0));
        table->setItem(0, 0, item);
        if (table->columnCount() > 1)
            table->setSpan(0, 0, 1, table->columnCount());
        m_currentRowIds.clear();
        return;
    }

    table->resizeColumnsToContents();
}

void HistoryDialog::fillRow(QTableWidget *table, int row,
                            const HistoryEntry &entry)
{
    if (!table)
        return;

    const QString timeText = entry.createdAt.isValid()
                                 ? entry.createdAt.toString(
                                       QString::fromLatin1(kTimeFormat))
                                 : QString();

    auto makeItem = [](const QString &text) {
        auto *item = new QTableWidgetItem(text);
        item->setToolTip(text);   // 列宽有限，长内容靠 tooltip 看全
        return item;
    };

    int col = 0;

    // 第一列永远是时间
    table->setItem(row, col++, makeItem(timeText));

    switch (entry.category) {
    case HistoryCategory::Conversation: {
        // 列：时间 | 角色 | 内容
        table->setItem(row, col++, makeItem(entry.subtitle));
        table->setItem(row, col++, makeItem(entry.title));
        break;
    }
    case HistoryCategory::Task: {
        // 列：时间 | 命令 | 状态 | 结果
        table->setItem(row, col++, makeItem(entry.title));
        table->setItem(row, col++, makeItem(entry.subtitle));
        table->setItem(row, col++, makeItem(
            entry.payload.value(QStringLiteral("result_summary")).toString()));
        break;
    }
    case HistoryCategory::Translation: {
        // 列：时间 | 方向 | 原文 | 译文
        table->setItem(row, col++, makeItem(entry.subtitle));
        table->setItem(row, col++, makeItem(entry.title));
        table->setItem(row, col++, makeItem(
            entry.payload.value(QStringLiteral("translated_text")).toString()));
        break;
    }
    case HistoryCategory::Search: {
        // 列：时间 | 关键词 | 服务
        table->setItem(row, col++, makeItem(entry.title));
        table->setItem(row, col++, makeItem(entry.subtitle));
        break;
    }
    case HistoryCategory::Recording: {
        // 列：时间 | 文件 | 时长 | 状态
        table->setItem(row, col++, makeItem(entry.title));
        const int seconds = entry.subtitle.toInt();
        table->setItem(row, col++, makeItem(QStringLiteral("%1:%2")
            .arg(seconds / 60, 2, 10, QLatin1Char('0'))
            .arg(seconds % 60, 2, 10, QLatin1Char('0'))));
        table->setItem(row, col++, makeItem(
            entry.payload.value(QStringLiteral("deleted")).toBool()
                ? I18n::instance().text("history.recording.deleted")
                : I18n::instance().text("history.recording.active")));
        break;
    }
    }

    // 把 id 记在第一列上，删除时取用
    if (QTableWidgetItem *first = table->item(row, 0))
        first->setData(kIdRole, entry.id);
    else if (auto *created = new QTableWidgetItem()) {
        created->setData(kIdRole, entry.id);
        table->setItem(row, 0, created);
    }

    while (m_currentRowIds.size() <= row)
        m_currentRowIds.append(-1);
    m_currentRowIds[row] = entry.id;
}

// ---------------------------------------------------------------------------
// 统计
// ---------------------------------------------------------------------------
void HistoryDialog::updateStats()
{
    if (!m_statsLabel)
        return;

    if (!m_history || !m_history->isAvailable()) {
        m_statsLabel->setText(I18n::instance().text("history.stats.unavailable"));
        m_statsLabel->setStyleSheet(QStringLiteral("color:#7A5A00;"));
        return;
    }

    const HistoryStorageStats stats = m_history->storageStats();

    const double usedMb = double(stats.totalBytes) / (1024.0 * 1024.0);
    const double limitMb = double(stats.maxStorageBytes) / (1024.0 * 1024.0);

    QString text = I18n::instance().text(
        "history.stats.summary",
        QStringLiteral("used"), QString::number(usedMb, 'f', 1),
        QStringLiteral("limit"), QString::number(limitMb, 'f', 0));

    text += QStringLiteral("  ·  ");
    text += I18n::instance().text(
        "history.stats.counts",
        QStringLiteral("conv"), QString::number(stats.conversationCount),
        QStringLiteral("task"), QString::number(stats.taskCount));
    text += QStringLiteral("  ·  ");
    text += I18n::instance().text(
        "history.stats.counts2",
        QStringLiteral("trans"), QString::number(stats.translationCount),
        QStringLiteral("search"), QString::number(stats.searchCount));
    text += QStringLiteral("  ·  ");
    text += I18n::instance().text(
        "history.stats.recordings",
        QStringLiteral("count"), QString::number(stats.recordingCount));

    // 接近上限时提醒（SRS 4.10：最大占用空间可配置）
    if (stats.usageRatio >= 0.9) {
        m_statsLabel->setStyleSheet(QStringLiteral("color:#C62828;"));
        text += QStringLiteral("  ⚠ ");
        text += I18n::instance().text("history.stats.near_limit");
    } else if (stats.usageRatio >= 0.75) {
        m_statsLabel->setStyleSheet(QStringLiteral("color:#B26A00;"));
    } else {
        m_statsLabel->setStyleSheet(QStringLiteral("color:#5A6472;"));
    }

    m_statsLabel->setText(text);
}

HistoryStorageStats HistoryDialog::storageStats() const
{
    if (!m_history)
        return {};
    return m_history->storageStats();
}

// ---------------------------------------------------------------------------
// 检索
// ---------------------------------------------------------------------------
void HistoryDialog::onSearchTriggered()
{
    m_filtered = true;

    const QString keyword = m_searchInput ? m_searchInput->text().trimmed() : QString();

    // 关键词为空但设了时间范围，这是合法用法（"看看这周都干了什么"）
    if (keyword.isEmpty() && m_fromEdit && m_toEdit) {
        if (m_fromEdit->dateTime() >= m_toEdit->dateTime()) {
            QMessageBox::information(
                this, I18n::instance().text("history.title"),
                I18n::instance().text("history.search.invalid_range"));
            return;
        }
    }

    DC_INFO(LogCat::History,
            QStringLiteral("History search executed (keyword provided=%1)")
                .arg(keyword.isEmpty() ? QStringLiteral("no")
                                       : QStringLiteral("yes")));

    populateCurrentTab();
}

void HistoryDialog::onClearSearch()
{
    m_filtered = false;

    if (m_searchInput)
        m_searchInput->clear();

    populateCurrentTab();
}

// ---------------------------------------------------------------------------
// 删除
// ---------------------------------------------------------------------------
QVector<qint64> HistoryDialog::selectedIds() const
{
    QVector<qint64> ids;

    QTableWidget *table =
        m_tables.value(static_cast<int>(currentCategory()), nullptr);
    if (!table)
        return ids;

    const auto ranges = table->selectedRanges();
    for (const auto &range : ranges) {
        for (int row = range.topRow(); row <= range.bottomRow(); ++row) {
            QTableWidgetItem *item = table->item(row, 0);
            if (!item)
                continue;
            const qint64 id = item->data(kIdRole).toLongLong();
            if (id > 0 && !ids.contains(id))
                ids.append(id);
        }
    }

    return ids;
}

void HistoryDialog::onDeleteSelected()
{
    if (!m_history || !m_history->isAvailable())
        return;

    const QVector<qint64> ids = selectedIds();
    if (ids.isEmpty()) {
        QMessageBox::information(
            this, I18n::instance().text("history.title"),
            I18n::instance().text("history.delete.select_first"));
        return;
    }

    if (!confirm(I18n::instance().text("history.delete.title"),
                 I18n::instance().text("history.delete.selected_confirm",
                                       QStringLiteral("count"),
                                       QString::number(ids.size())))) {
        return;
    }

    const HistoryCategory category = currentCategory();
    int removed = 0;
    int failed = 0;

    for (qint64 id : ids) {
        const Result r = m_history->removeById(category, id);
        if (r.ok)
            ++removed;
        else
            ++failed;
    }

    DC_INFO(LogCat::History,
            QStringLiteral("History deleted by user: removed=%1 failed=%2")
                .arg(removed).arg(failed));

    // 部分失败要说清楚 —— 只说"删除成功"是在骗用户
    if (failed > 0) {
        QMessageBox::warning(
            this, I18n::instance().text("history.title"),
            I18n::instance().text("history.delete.partial",
                                  QStringLiteral("removed"), QString::number(removed),
                                  QStringLiteral("failed"), QString::number(failed)));
    }

    populateCurrentTab();
    updateStats();
}

void HistoryDialog::onDeleteByRange()
{
    if (!m_history || !m_history->isAvailable())
        return;

    if (!m_fromEdit || !m_toEdit)
        return;

    const QDateTime from = m_fromEdit->dateTime();
    const QDateTime to = m_toEdit->dateTime();

    if (from >= to) {
        QMessageBox::information(
            this, I18n::instance().text("history.title"),
            I18n::instance().text("history.search.invalid_range"));
        return;
    }

    const QString fromText = from.toString(QString::fromLatin1(kTimeFormat));
    const QString toText = to.toString(QString::fromLatin1(kTimeFormat));

    if (!confirm(I18n::instance().text("history.delete.range_title"),
                 I18n::instance().text("history.delete.range_confirm",
                                       QStringLiteral("category"),
                                       categoryTitle(currentCategory()),
                                       QStringLiteral("range"),
                                       QStringLiteral("%1 ~ %2").arg(fromText, toText)))) {
        return;
    }

    const Result r = m_history->removeByTimeRange(currentCategory(), from, to);
    if (!r.ok) {
        QMessageBox::warning(this,
                             I18n::instance().text("history.title"),
                             r.message.isEmpty()
                                 ? I18n::instance().text("history.delete.failed")
                                 : r.message);
        return;
    }

    DC_INFO(LogCat::History,
            QStringLiteral("History removed by time range"));

    populateCurrentTab();
    updateStats();
}

void HistoryDialog::onClearCategory()
{
    if (!m_history || !m_history->isAvailable())
        return;

    const HistoryCategory category = currentCategory();
    const QString title = categoryTitle(category);

    // 清空一整类：要求用户输入类别名。
    //  这一步的摩擦是刻意的 —— 它让"手滑点两下"变成不可能。
    bool ok = false;
    const QString typed = QInputDialog::getText(
        this,
        I18n::instance().text("history.clear.category_title"),
        I18n::instance().text("history.clear.category_confirm",
                              QStringLiteral("category"), title),
        QLineEdit::Normal, QString(), &ok);

    if (!ok)
        return;

    if (typed.trimmed() != title.trimmed()) {
        QMessageBox::information(
            this, I18n::instance().text("history.title"),
            I18n::instance().text("history.clear.mismatch"));
        return;
    }

    const Result r = m_history->removeAll(category);
    if (!r.ok) {
        QMessageBox::warning(this,
                             I18n::instance().text("history.title"),
                             r.message.isEmpty()
                                 ? I18n::instance().text("history.delete.failed")
                                 : r.message);
        return;
    }

    DC_WARN(LogCat::History,
            QStringLiteral("History category cleared by user"));

    populateCurrentTab();
    updateStats();
}

void HistoryDialog::onClearEverything()
{
    if (!m_history || !m_history->isAvailable())
        return;

    // 最危险的操作：必须手动输入确认词。
    const QString keyword = I18n::instance().text("history.clear.keyword");

    bool ok = false;
    const QString typed = QInputDialog::getText(
        this,
        I18n::instance().text("history.clear.all_title"),
        I18n::instance().text("history.clear.all_confirm",
                              QStringLiteral("keyword"), keyword),
        QLineEdit::Normal, QString(), &ok);

    if (!ok)
        return;

    if (typed.trimmed() != keyword.trimmed()) {
        QMessageBox::information(
            this, I18n::instance().text("history.title"),
            I18n::instance().text("history.clear.mismatch"));
        return;
    }

    const Result r = m_history->removeEverything();
    if (!r.ok) {
        QMessageBox::warning(this,
                             I18n::instance().text("history.title"),
                             r.message.isEmpty()
                                 ? I18n::instance().text("history.delete.failed")
                                 : r.message);
        return;
    }

    DC_WARN(LogCat::History,
            QStringLiteral("ALL history cleared by explicit user action"));

    reload();
}

// ---------------------------------------------------------------------------
// 导出
// ---------------------------------------------------------------------------
void HistoryDialog::onExport()
{
    if (!m_history || !m_history->isAvailable())
        return;

    // 默认文件名带时间戳，避免用户连续导出互相覆盖
    const QString defaultName =
        QStringLiteral("desktop-companion-history-%1.json")
            .arg(QDateTime::currentDateTime().toString(
                QStringLiteral("yyyyMMdd-HHmmss")));

    const QString path = QFileDialog::getSaveFileName(
        this,
        I18n::instance().text("history.export.title"),
        defaultName,
        I18n::instance().text("history.export.filter"));

    if (path.isEmpty())
        return;   // 用户取消

    // 注意：导出格式由扩展名决定，和 HistoryService 的约定一致
    const QString format =
        path.endsWith(QStringLiteral(".csv"), Qt::CaseInsensitive)
            ? QStringLiteral("csv")
            : QStringLiteral("json");

    const Result r = m_history->exportAll(path, format);
    if (!r.ok) {
        QMessageBox::warning(this,
                             I18n::instance().text("history.title"),
                             r.message.isEmpty()
                                 ? I18n::instance().text("history.export.failed")
                                 : r.message);
        return;
    }

    DC_INFO(LogCat::History,
            QStringLiteral("History exported (format=%1)").arg(format));

    // 导出成功要给出完整路径：用户才知道文件到底在哪
    QMessageBox::information(
        this, I18n::instance().text("history.title"),
        I18n::instance().text("history.export.done",
                              QStringLiteral("path"), QDir::toNativeSeparators(path)));
}

// ---------------------------------------------------------------------------
// 确认与键盘
// ---------------------------------------------------------------------------
bool HistoryDialog::confirm(const QString &title, const QString &text)
{
    const auto answer = QMessageBox::question(
        this, title, text,
        QMessageBox::Yes | QMessageBox::No,
        QMessageBox::No);   // 默认"否"
    return answer == QMessageBox::Yes;
}

void HistoryDialog::closeEvent(QCloseEvent *event)
{
    event->accept();
    hide();   // 复用实例
}

void HistoryDialog::keyPressEvent(QKeyEvent *event)
{
    switch (event->key()) {
    case Qt::Key_Escape:
        hide();
        return;
    case Qt::Key_F5:
        // F5 刷新是通用直觉，顺手支持一下
        reload();
        return;
    case Qt::Key_Delete:
        // 删除键直接删选中项（仍然会二次确认）
        onDeleteSelected();
        return;
    default:
        break;
    }

    QDialog::keyPressEvent(event);
}

} // namespace dc
