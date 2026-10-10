// ============================================================================
//  HistoryDialog.h — 历史记录窗口
//
//  需求依据：SRS V1.1 第 4.10 节（历史记录与隐私）
//    - 聊天、翻译、搜索和任务历史可分别启用或关闭
//    - 提供检索功能
//    - 提供按条删除、按时间范围删除、全部清空功能
//    - 提供导出功能
//    - 提供保留期限与最大占用空间设置
//    - 本地存储优先使用 SQLite
//
//  关键设计决定：
//
//    1. 【关掉的类别不是"灰掉"，而是"不出现标签页"】
//       SRS 4.10 说五类历史"可分别启用或关闭"，而关闭的含义是
//       "不记录"。既然根本没记录，显示一个空标签页只会让用户困惑
//       （"我明明关了啊，怎么还有个空的？"）。
//       所以被关闭的类别直接不建标签页，并在底部说明当前记录范围。
//
//    2. 【删除一律二次确认，且"全部清空"要输入确认词】
//       历史记录是不可恢复的用户数据。按条删除是一次确认；
//       清空整类要输入类别名；"清空全部"必须手动输入一个
//       明确的确认词。摩擦程度与破坏力成正比。
//
//    3. 【导出不包含密钥，且要告诉用户文件写到哪了】
//       SRS 5 规定密钥不随配置导出。历史导出同样不该带任何凭据。
//       导出完成后显示完整路径，用户才知道去哪拿文件。
//
//    4. 【借调 HistoryService 做检索，不在界面里做过滤】
//       界面里对已加载的 100 条做过滤，会让用户以为"只有 100 条"。
//       真正正确的做法是把关键词交给 SQL 层（HistoryService 已实现），
//       这样搜的是全量数据。界面只负责传参和展示。
//
//    5. 【不做"自动刷新"】
//       历史窗口打开时不做定时轮询。原因：这个窗口是"回看"用的，
//       不是监控面板；轮询既费电又会在用户滚动时跳动。
//       需要最新数据就点刷新，或重开窗口。
// ============================================================================
#pragma once

#include "model/HistoryRecord.h"
#include "model/Types.h"

// 直接包含而不是前置声明：storageStats() 按值返回 HistoryStorageStats，
//  按值返回需要完整定义，前置声明不够。
//  （这个坑在 CoreRuntime.h 里也踩过一次，见那里的注释。）
#include "services/HistoryService.h"

#include <QDialog>
#include <QHash>
#include <QString>
#include <QVector>

QT_BEGIN_NAMESPACE
class QComboBox;
class QDateTimeEdit;
class QLabel;
class QLineEdit;
class QPushButton;
class QTabWidget;
class QTableWidget;
QT_END_NAMESPACE

namespace dc {

// ---------------------------------------------------------------------------
// HistoryDialog
// ---------------------------------------------------------------------------
class HistoryDialog : public QDialog
{
    Q_OBJECT

public:
    explicit HistoryDialog(HistoryService *history, QWidget *parent = nullptr);
    ~HistoryDialog() override;

    // 打开窗口：刷新统计与当前标签页数据后显示
    void open();

    // 重新加载当前标签页
    void reload();

    // 存储统计展示（供测试）
    HistoryStorageStats storageStats() const;

protected:
    void closeEvent(QCloseEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;

private slots:
    void onTabChanged(int index);
    void onSearchTriggered();
    void onClearSearch();
    void onDeleteSelected();
    void onDeleteByRange();
    void onClearCategory();
    void onClearEverything();
    void onExport();

private:
    // 建界面
    void buildUi();
    // 按隐私设置决定建哪些标签页
    void buildTabs();
    // 底层数据 -> 表格
    void populateCurrentTab();
    // 更新底部统计文字与容量条
    void updateStats();

    // 当前标签页对应的类别
    HistoryCategory currentCategory() const;

    // 当前检索条件
    HistoryQuery currentQuery() const;

    // 取某个类别在表格里选中行的记录 id
    QVector<qint64> selectedIds() const;

    // 类别 -> 标签页标题
    static QString categoryTitle(HistoryCategory category);
    // 表格列头
    static QStringList headersFor(HistoryCategory category);

    // 把一条统一记录填进表格行
    void fillRow(QTableWidget *table, int row, const HistoryEntry &entry);

    // 确认对话框
    bool confirm(const QString &title, const QString &text);

    HistoryService *m_history = nullptr;

    QTabWidget *m_tabs = nullptr;
    QLineEdit *m_searchInput = nullptr;
    QDateTimeEdit *m_fromEdit = nullptr;
    QDateTimeEdit *m_toEdit = nullptr;
    QLabel *m_statsLabel = nullptr;
    QLabel *m_privacyNoteLabel = nullptr;

    QPushButton *m_searchButton = nullptr;
    QPushButton *m_clearSearchButton = nullptr;
    QPushButton *m_deleteButton = nullptr;
    QPushButton *m_deleteRangeButton = nullptr;
    QPushButton *m_clearCategoryButton = nullptr;
    QPushButton *m_clearAllButton = nullptr;
    QPushButton *m_exportButton = nullptr;
    QPushButton *m_refreshButton = nullptr;
    QPushButton *m_closeButton = nullptr;

    // 每个类别一个表格；不存在的类别不出现在这里
    QHash<int, QTableWidget *> m_tables;

    // 当前表格里每行对应的记录 id
    QVector<qint64> m_currentRowIds;

    // 当前是否在检索状态
    bool m_filtered = false;
};

} // namespace dc
