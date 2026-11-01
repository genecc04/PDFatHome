#pragma once
#include <QHash>
#include <QList>
#include <QMainWindow>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <QThreadPool>
#include <atomic>
#include <memory>

#include "EditHistory.h"
#include "PageView.h"
#include "PdfEditor.h"

class QAction;
class QCloseEvent;
class QDockWidget;
class QDragEnterEvent;
class QDropEvent;
class QLabel;
class QMenu;
class QSpinBox;
class BookmarkPane;
class SearchPane;
class ThumbnailPane;

class MainWindow : public QMainWindow
{
public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

    void openPath(const QString &path);

protected:
    void closeEvent(QCloseEvent *event) override;
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dropEvent(QDropEvent *event) override;

private:
    struct DocumentLoadResult;
    class EditOperationGuard
    {
    public:
        explicit EditOperationGuard(MainWindow *window);
        ~EditOperationGuard();
        EditOperationGuard(const EditOperationGuard &) = delete;
        EditOperationGuard &operator=(const EditOperationGuard &) = delete;

    private:
        MainWindow *m_window;
    };

    // setup
    void setupViewer();
    void setupThumbnailPane();
    void setupSearchPane();
    void setupBookmarkPane();
    void setupMenus();
    void setupToolbar();
    void setupImageToolbar();
    void setupTextToolbar();

    // file and viewing
    void openFile();
    void loadDocument(const QString &path);
    void startDocumentLoad(const QString &path, const QString &password,
                           bool firstAttempt, const QString &workingPath);
    void finishDocumentLoad(const QString &path, const QString &password,
                            bool firstAttempt, const QString &workingPath,
                            const std::shared_ptr<DocumentLoadResult> &result);
    bool save();
    bool saveAs();
    void optimizePdf();
    void applyRedactions();
    void goToPage(int page);
    bool showFile(const QString &path, int page, bool reportErrors = true,
                  bool preserveRasterPreviews = false, bool forceReload = false);
    void clearPageRenderCaches();
    void addRecentFile(const QString &path);
    void setDocumentPassword();
    void removeDocumentPassword();
    bool isDocumentModified() const;
    bool isPasswordProtected() const;
    PdfEditor::Encryption saveEncryption() const;
    bool openWorkingCopy(const QString &source, int page);
    void updateRecentMenu();
    void showAbout();

    // editing
    void rotateSelectedPages(int degrees);
    void deleteSelectedPages();
    void movePage(int from, int to);
    void extractPages();
    void insertPages();
    void highlightText();
    void placeText(int page, const QRectF &box, const QString &text,
                   const PageView::TextStyle &style);
    void updateTextAnnotation(int page, int annotationId,
                              const QString &oldText, const QString &newText,
                              const PageView::TextStyle &style);
    void updateTextAnnotationBounds(int page, int annotationId, const QString &text,
                                    const QRectF &oldBounds, const QRectF &newBounds,
                                    const PageView::TextStyle &style);
    void deleteTextAnnotation(int page, int annotationId, const QString &text);
    void updateImageStampBounds(int page, const QString &id, const QRectF &oldBounds,
                                const QRectF &newBounds);
    void deleteImageStamp(int page, const QString &id);
    bool refreshAfterEdit(int page, const QList<int> &reselect = {},
                          const QVector<QSizeF> &pageSizes = {},
                          bool preserveRasterPreviews = false);
    bool refreshAfterPageEdit(const QList<int> &changedPages, int page);
    QList<int> targetPages() const;
    void setupDrawToolbar();
    void placeStrokes(const QVector<PageView::Stroke> &strokes);
    void eraseInk(int page, const QVector<QPointF> &path, double radius);
    void addImage();
    void placeImage(int page, const QRectF &box, const QImage &image);
    void exportPages(const QString &format);
    bool fillFormField(int page, const QPointF &point, PageView::FormField *field);
    bool commitFormField(int page, int widgetIndex, const QString &value);
    bool refreshFormFieldOverlays(int firstPage = -1, int lastPage = -1);
    void undo();
    void redo();
    void stepHistory(const QString &path);

    // document state
    bool confirmDiscard();
    void updateTitle();
    void updateActions();
    void setBusyCursor(bool busy);
    void beginEditOperation();
    void endEditOperation();
    bool reopenEditor(const QString &path);
    void removeSnapshots(const QStringList &paths);
    void removeDeferredSnapshots();
    bool removeTempFile(const QString &path, bool reportFailure = false);
    bool isTempFile(const QString &path) const;

    
    enum class PasswordChange { None, Set, Remove };
    PasswordChange m_passwordChange = PasswordChange::None;

    PdfEditor *m_document = nullptr;
    PageView *m_view = nullptr;
    QSpinBox *m_pageBox = nullptr;
    QLabel *m_pageCountLabel = nullptr;
    ThumbnailPane *m_thumbnails = nullptr;
    QDockWidget *m_pagesDock = nullptr;
    SearchPane *m_search = nullptr;
    QDockWidget *m_searchDock = nullptr;
    BookmarkPane *m_bookmarks = nullptr;
    QDockWidget *m_bookmarksDock = nullptr;
    QMenu *m_recentMenu = nullptr;

    QAction *m_saveAction = nullptr;
    QAction *m_saveAsAction = nullptr;
    QAction *m_optimizeAction = nullptr;
    QAction *m_redactAction = nullptr;
    QAction *m_applyRedactionsAction = nullptr;
    QAction *m_clearRedactionsAction = nullptr;
    QAction *m_extractAction = nullptr;
    QAction *m_exportPngAction = nullptr;
    QAction *m_exportSvgAction = nullptr;
    QAction *m_insertAction = nullptr;
    QAction *m_undoAction = nullptr;
    QAction *m_redoAction = nullptr;
    QAction *m_highlightAction = nullptr;
    QAction *m_addTextAction = nullptr;
    QAction *m_deleteTextAction = nullptr;
    QAction *m_deleteImageAction = nullptr;
    QAction *m_rotateLeftAction = nullptr;
    QAction *m_rotateRightAction = nullptr;
    QAction *m_deletePageAction = nullptr;
    QAction *m_drawAction = nullptr;
    QAction *m_addImageAction = nullptr;
    QAction *m_cropImageAction = nullptr;
    QAction *m_cancelImageAction = nullptr;
    QString m_newPassword;
    QAction *m_setPasswordAction = nullptr;
    QAction *m_removePasswordAction = nullptr;

    std::unique_ptr<PdfEditor> m_editor;
    std::atomic_bool m_closing{false};
    QThreadPool m_documentLoadPool;
    bool m_documentLoadPending = false;
    bool m_documentOperationPending = false;
    int m_editOperationDepth = 0;
    quint64 m_editOperationGeneration = 0;
    bool m_busyCursorActive = false;
    EditHistory m_history;
    QTemporaryDir m_tempDir;
    QString m_currentPath;
    QString m_displayName;
    QString m_password;
    QStringList m_deferredSnapshotRemovals;
    int m_editCount = 0;
    QSet<int> m_formFieldPagesLoaded;
    QSet<int> m_formFieldPagesFailed;
    QHash<QString, QVector<QSizeF>> m_pageSizeCache;
};