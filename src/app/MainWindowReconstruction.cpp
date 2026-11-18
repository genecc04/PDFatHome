#include "MainWindow.h"
#include "PdfEditor.h"
#include "ReconstructionController.h"
#include "ReconstructionOverlay.h"
#include "SystemFonts.h"

#include "reconstruction/AutoReconstruct.h"
#include "reconstruction/RichText.h"

#include <QAction>
#include <QColor>
#include <QHash>
#include <QMap>
#include <QMessageBox>
#include <QStatusBar>
#include <QStringList>
#include <QVector>

#include <utility>


void MainWindow::setupReconstruction()
{
    m_reconstruction = new ReconstructionController(this);
    ReconstructionOverlay *overlay = m_view->reconstruction();

    m_reconstructAction = new QAction("Show text &regions (reconstruct)", this);
    m_reconstructAction->setCheckable(true);
    m_reconstructAction->setEnabled(false);
    m_reconstructAction->setToolTip(
        "Find the text on this page and outline it. Click a region to edit it. Uses the "
        "page's own text when it has any, and OCR (text recognition) when it does not. "
        "Edits wait until you choose Apply reconstruction.");

    m_applyReconstructionAction = new QAction("&Apply reconstruction", this);
    m_applyReconstructionAction->setEnabled(false);
    m_applyReconstructionAction->setToolTip(
        "Write every text edit made in reconstruct mode into the PDF at once, then leave "
        "reconstruct mode. Leaving reconstruct mode and saving write the edits too.");

    connect(m_applyReconstructionAction, &QAction::triggered, this, [this]() {
        applyReconstruction();

        if (!m_reconstructAction->isChecked()
            || m_view->reconstruction()->pendingCount() > 0)
            return;

        const QString message = statusBar()->currentMessage();
        m_reconstructAction->setChecked(false);
        if (!message.isEmpty())
            statusBar()->showMessage(message, 8000);
    });

    m_discardReconstructionAction = new QAction("Discard reconstruction &edits", this);
    m_discardReconstructionAction->setEnabled(false);
    m_discardReconstructionAction->setToolTip(
        "Throw away the reconstruct-mode text edits that have not been applied yet");
    connect(m_discardReconstructionAction, &QAction::triggered,
            this, &MainWindow::discardReconstruction);

    connect(m_reconstructAction, &QAction::toggled, this, [this, overlay](bool on) {
        if (on) {
            m_view->commitPendingEdits();
            overlay->setActive(true);
            requestReconstruction();
        } else {
            overlay->setActive(false);
            statusBar()->clearMessage();
        }
    });

    connect(m_view, &PageView::currentPageChanged, this, [this](int) {
        if (m_reconstructAction->isChecked())
            requestReconstruction();
    });

    connect(m_reconstruction, &ReconstructionController::pageReady, this,
            [this, overlay](int page,
                            std::shared_ptr<const Reconstruction::ReconstructedPage> result,
                            int source, const QString &note) {
                if (!m_reconstructAction->isChecked() || page != m_view->currentPage())
                    return;

                overlay->setResult(page, result, source);

                QString message =
                    QString("%1: %2 text region(s) on page %3")
                        .arg(source == int(Reconstruction::PageSource::Ocr)
                                 ? QStringLiteral("OCR")
                                 : QStringLiteral("Page text"))
                        .arg(result->paragraphs.size())
                        .arg(page + 1);
                if (!note.isEmpty())
                    message += " - " + note;
                if (overlay->pendingCount() > 0)
                    message += QString(" - %1 edit(s) waiting to be applied")
                                   .arg(overlay->pendingCount());
                statusBar()->showMessage(message, 10000);
            });

    connect(m_reconstruction, &ReconstructionController::failed, this,
            [this](int page, const QString &error) {
                if (m_reconstructAction->isChecked() && page == m_view->currentPage())
                    statusBar()->showMessage("Could not reconstruct page "
                                                 + QString::number(page + 1) + ": " + error,
                                             10000);
            });

    connect(overlay, &ReconstructionOverlay::pendingChanged, this, [this](int count) {
        updateActions();
        updateTitle();
        if (count > 0)
            statusBar()->showMessage(
                QString("%1 text edit(s) waiting. Choose Apply reconstruction to write "
                        "them into the PDF.").arg(count),
                8000);
    });
    connect(overlay, &ReconstructionOverlay::editingChanged, this,
            [this](bool) { updateActions(); });
    connect(overlay, &ReconstructionOverlay::applyRequested,
            this, &MainWindow::applyReconstruction);
}

void MainWindow::requestReconstruction()
{
    if (!m_editor || !m_reconstruction)
        return;

    statusBar()->showMessage("Finding text on this page...");
    m_reconstruction->requestPage(m_view->currentPage());
}

void MainWindow::updateReconstructionSource(const DocumentSource &source)
{
    if (!m_reconstruction)
        return;

    m_reconstruction->setDocument(source);

    if (m_reconstructAction && m_reconstructAction->isChecked())
        requestReconstruction();
}

void MainWindow::applyReconstruction()
{
    if (!m_editor || m_viewOnly)
        return;

    ReconstructionOverlay *overlay = m_view->reconstruction();
    QVector<PendingReconstructionEdit> pending = overlay->takePending();
    if (pending.isEmpty())
        return;

    overlay->reset();

    constexpr bool kEmbedSystemFonts = true;
    QHash<QString, QByteArray> fontFiles;
    QStringList notEmbedded;

    QMap<int, QVector<PdfEditor::TextReplacement>> byPage;
    for (const PendingReconstructionEdit &item : std::as_const(pending)) {
        const ReconstructionEdit &edit = item.edit;
        const QVector<Reconstruction::PlacedSpan> placed =
            Reconstruction::placeSpans(edit.paragraph, edit.region, edit.firstBaselineY);

        PdfEditor::TextReplacement replacement;
        replacement.eraseArea = edit.paragraph.bounds;
        replacement.fillColor = edit.fromTextLayer ? QColor() : edit.paragraph.backgroundColor;
        replacement.removeOriginalText = edit.fromTextLayer;

        for (const Reconstruction::PlacedSpan &piece : placed) {
            PdfEditor::TextSpan span;
            span.text = piece.text;
            span.font = piece.baseFont;
            span.sizePoints = piece.sizePoints;
            span.color = piece.color;
            span.underlined = piece.underlined;
            span.origin = piece.origin;
            span.width = piece.width;

            if (kEmbedSystemFonts && !piece.systemFamily.isEmpty()) {
                const QString key = QStringLiteral("%1|%2|%3")
                                        .arg(piece.systemFamily)
                                        .arg(int(piece.bold))
                                        .arg(int(piece.italic));
                auto found = fontFiles.find(key);
                if (found == fontFiles.end()) {
                    found = fontFiles.insert(key, SystemFonts::embeddableFontFile(
                                                      piece.systemFamily, piece.bold,
                                                      piece.italic));
                }
                if (!found.value().isEmpty()) {
                    span.fontFile = found.value();
                    span.fontKey = key;
                } else if (!notEmbedded.contains(piece.systemFamily)) {
                    notEmbedded << piece.systemFamily;
                }
            }
            replacement.spans.append(span);
        }
        byPage[edit.page].append(replacement);
    }

    EditOperationGuard operation(this);

    auto giveUp = [&]() {
        reopenEditor(m_history.current());
        overlay->restorePending(pending);
        if (m_reconstructAction->isChecked())
            requestReconstruction();
        updateActions();
    };

    int unsupported = 0;
    QList<int> pages;
    for (auto it = byPage.cbegin(); it != byPage.cend(); ++it) {
        int count = 0;
        if (!m_editor->replaceTexts(it.key(), it.value(), &count)) {
            QMessageBox::warning(this, "Apply reconstruction failed",
                                 QString("Page %1: %2").arg(it.key() + 1)
                                     .arg(m_editor->lastError()));
            giveUp();
            return;
        }
        unsupported += count;
        pages.append(it.key());
    }

    if (!refreshAfterPageEdit(pages, m_view->currentPage())) {
        overlay->restorePending(pending);
        if (m_reconstructAction->isChecked())
            requestReconstruction();
        return;
    }

    QString message = QString("Applied %1 text edit(s) on %2 page(s).")
                          .arg(int(pending.size()))
                          .arg(int(pages.size()));
    if (unsupported > 0)
        message += QString(" %1 character(s) can't be shown in the standard PDF fonts and "
                           "were replaced with \"?\".").arg(unsupported);
    if (!notEmbedded.isEmpty())
        message += QString(" %1 can't be embedded in a PDF (variable, OpenType/CFF or "
                           "restricted font), so a standard PDF font was used instead.")
                       .arg(notEmbedded.join(QStringLiteral(", ")));
    statusBar()->showMessage(message, 8000);
}

void MainWindow::discardReconstruction()
{
    ReconstructionOverlay *overlay = m_view->reconstruction();
    const int count = overlay->pendingCount();
    if (count == 0)
        return;

    const auto answer = QMessageBox::question(
        this, "Discard reconstruction edits",
        QString("Discard %1 text edit(s) that have not been applied?").arg(count),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (answer != QMessageBox::Yes)
        return;

    overlay->discardPending();
    statusBar()->showMessage("Discarded the pending reconstruction edits.", 4000);
}