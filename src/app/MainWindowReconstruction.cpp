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

// "Reconstruct" mode. Three objects cooperate, and this file is the glue:
//
//   ReconstructionController  works out a page's paragraphs on a background
//                             thread (text layer first, OCR if there is none)
//   ReconstructionOverlay     lives inside PageView: outlines paragraphs,
//                             highlights the hovered one, opens the editor box,
//                             and keeps finished edits as "pending" (drawn as a
//                             preview) instead of writing them
//   MainWindow (this file)    switches the overlay on/off, hands it each result,
//                             and writes ALL pending edits into the PDF at once
//
// Edits are written when the user chooses Apply reconstruction (which also leaves
// the mode), leaves the mode,
// saves, or does anything else that changes the document (those all go through
// PageView::commitPendingEdits -> ReconstructionOverlay::flush -> applyRequested).

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

    // Only the button leaves the mode. Saving and other edits also write the pending
    // edits (through PageView::commitPendingEdits -> applyRequested), and those must
    // not throw the user out of the mode, so the exit lives here and not in
    // applyReconstruction().
    connect(m_applyReconstructionAction, &QAction::triggered, this, [this]() {
        applyReconstruction();

        // Pending edits left over means the write failed and they were put back:
        // stay in the mode so the user can retry or fix them.
        if (!m_reconstructAction->isChecked()
            || m_view->reconstruction()->pendingCount() > 0)
            return;

        // Leaving the mode clears the status bar, which would wipe the "Applied ..."
        // message, so show it again afterwards.
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
            // The overlay ignores results until it is active, so this has to
            // come BEFORE the request (a cached page answers immediately).
            overlay->setActive(true);
            requestReconstruction();
        } else {
            overlay->setActive(false); // applies the pending edits first
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
                // Results can arrive after the user scrolled away or switched it off.
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

// Called whenever the file the viewer shows changes (open, edit, undo, save).
void MainWindow::updateReconstructionSource(const DocumentSource &source)
{
    if (!m_reconstruction)
        return;

    m_reconstruction->setDocument(source);

    if (m_reconstructAction && m_reconstructAction->isChecked())
        requestReconstruction();
}

// Writes every pending edit into the PDF, on however many pages.
//
//   1. placeSpans() lays each edit's text out exactly as the editor box showed it
//      and returns pieces with absolute positions (page points).
//   2. replaceTexts() does one page at a time: it erases ALL the old text first,
//      then draws the new pieces as real PDF text, so the result stays
//      selectable and searchable.
//   3. refreshAfterPageEdit() snapshots the file once (a single undo step for the
//      whole batch) and re-renders the changed pages.
void MainWindow::applyReconstruction()
{
    if (!m_editor || m_viewOnly)
        return;

    ReconstructionOverlay *overlay = m_view->reconstruction();
    QVector<PendingReconstructionEdit> pending = overlay->takePending();
    if (pending.isEmpty())
        return;

    // The document is about to change: drop the page overlay so a click can't act
    // on stale paragraphs. It is rebuilt when the new page text arrives.
    overlay->reset();

    // The PDF's own font, when this computer has it as a plain TrueType file that
    // may be embedded. Anything else keeps its standard font, and so does a font
    // that fails to embed. Set to false to always use the standard fonts.
    constexpr bool kEmbedSystemFonts = true;
    QHash<QString, QByteArray> fontFiles; // by key; an empty value means "not available"
    QStringList notEmbedded;              // installed fonts that could not be embedded

    QMap<int, QVector<PdfEditor::TextReplacement>> byPage;
    for (const PendingReconstructionEdit &item : std::as_const(pending)) {
        const ReconstructionEdit &edit = item.edit;
        const QVector<Reconstruction::PlacedSpan> placed =
            Reconstruction::placeSpans(edit.paragraph, edit.region, edit.firstBaselineY);

        PdfEditor::TextReplacement replacement;
        replacement.eraseArea = edit.paragraph.bounds;
        // A page with a real text layer: the old glyphs are removed, so no background
        // rectangle is painted (that would also hide a photo or colored shape behind
        // the text). A scan has no glyphs to remove, so its old text is covered with
        // the estimated background color instead.
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

    // Reconstruct mode stays on through this: the guard's updateActions() leaves
    // it alone.
    EditOperationGuard operation(this);

    // If writing fails, the document goes back to its last saved state and the
    // user's edits are put back as pending instead of being lost.
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
        // refreshAfterPageEdit already reported the error and reloaded the document.
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

// Used by Save and Save As: writes the waiting reconstruct edits and leaves the mode.
// Switching the mode off writes the edits first (ReconstructionOverlay::setActive ->
// flush -> applyRequested -> applyReconstruction). With nothing waiting it does nothing,
// so Ctrl+S after only looking at a page does not throw you out of the mode.
// Returns false when the edits could not be written: they are back in the overlay, the
// warning has been shown, and the caller should not save.
bool MainWindow::applyAndLeaveReconstruction()
{
    ReconstructionOverlay *overlay = m_view->reconstruction();
    if (m_reconstructAction->isChecked() && overlay->hasPendingEdit())
        m_reconstructAction->setChecked(false);
    return overlay->pendingCount() == 0;
}