#include "PdfEditor.h"

#include <QByteArray>
#include <QList>
#include <QSet>

#include <cmath>
#include <vector>

extern "C" {
#include <mupdf/fitz.h>
#include <mupdf/pdf.h>
}

namespace {

int winAnsiByte(uint ch)
{
    if (ch >= 0x20 && ch <= 0x7E)
        return int(ch);
    if (ch >= 0xA0 && ch <= 0xFF)
        return int(ch);
    if (ch == 0x09 || ch == 0x0A || ch == 0x0D)
        return 0x20;

    switch (ch) {
    case 0x20AC: return 0x80; case 0x201A: return 0x82; case 0x0192: return 0x83;
    case 0x201E: return 0x84; case 0x2026: return 0x85; case 0x2020: return 0x86;
    case 0x2021: return 0x87; case 0x02C6: return 0x88; case 0x2030: return 0x89;
    case 0x0160: return 0x8A; case 0x2039: return 0x8B; case 0x0152: return 0x8C;
    case 0x017D: return 0x8E; case 0x2018: return 0x91; case 0x2019: return 0x92;
    case 0x201C: return 0x93; case 0x201D: return 0x94; case 0x2022: return 0x95;
    case 0x2013: return 0x96; case 0x2014: return 0x97; case 0x02DC: return 0x98;
    case 0x2122: return 0x99; case 0x0161: return 0x9A; case 0x203A: return 0x9B;
    case 0x0153: return 0x9C; case 0x017E: return 0x9E; case 0x0178: return 0x9F;
    default: return -1;
    }
}

QByteArray encodeWinAnsi(const QString &text, int *unsupported)
{
    QByteArray bytes;
    const QList<uint> chars = text.toUcs4();
    for (const uint ch : chars) {
        const int b = winAnsiByte(ch);
        if (b < 0) {
            bytes += '?';
            ++*unsupported;
        } else {
            bytes += char(b);
        }
    }
    return bytes;
}

QByteArray num(double value)
{
    return QByteArray::number(value, 'f', 4);
}

QByteArray colorOps(const QColor &color, const char *op)
{
    return num(color.redF()) + ' ' + num(color.greenF()) + ' ' + num(color.blueF()) + ' ' + op + '\n';
}

QByteArray resourceNameFor(const QString &baseFont)
{
    QString name = baseFont;
    name.remove(QLatin1Char('-'));
    return "PHF" + name.toLatin1();
}

QByteArray resourceNameFor(const PdfEditor::TextSpan &span)
{
    if (span.fontFile.isEmpty())
        return resourceNameFor(span.font);

    QByteArray name = "PHE";
    for (const QChar ch : span.fontKey) {
        if (ch.unicode() < 128 && ch.isLetterOrNumber())
            name += char(ch.unicode());
    }
    return name + QByteArray::number(qulonglong(qHash(span.fontKey)), 16);
}

QByteArray polygon(const fz_matrix &inverse, const QRectF &rect)
{
    const QPointF corners[4] = { rect.topLeft(), rect.topRight(),
                                 rect.bottomRight(), rect.bottomLeft() };
    QByteArray out;
    for (int i = 0; i < 4; ++i) {
        const fz_point p = fz_transform_point(
            fz_make_point(float(corners[i].x()), float(corners[i].y())), inverse);
        out += num(p.x) + ' ' + num(p.y) + (i == 0 ? " m\n" : " l\n");
    }
    out += "h\n";
    return out;
}

QByteArray buildContent(const fz_matrix &inverse,
                        const QVector<PdfEditor::TextReplacement> &items, int *unsupported)
{
    QByteArray out = "q\n";

    for (const PdfEditor::TextReplacement &item : items) {
        if (!item.fillColor.isValid())
            continue;
        out += colorOps(item.fillColor, "rg");
        out += polygon(inverse, item.eraseArea.adjusted(-1.0, -1.0, 1.0, 1.0));
        out += "f\n";
    }

    QVector<PdfEditor::TextSpan> spans;
    for (const PdfEditor::TextReplacement &item : items)
        spans += item.spans;

    for (const PdfEditor::TextSpan &span : spans) {
        const QByteArray bytes = encodeWinAnsi(span.text, unsupported);
        if (bytes.trimmed().isEmpty())
            continue;

        fz_matrix tm = fz_concat(
            fz_make_matrix(1.0f, 0.0f, 0.0f, -1.0f, float(span.origin.x()),
                           float(span.origin.y())),
            inverse);

        if (span.syntheticItalic)
            tm = fz_concat(fz_make_matrix(1.0f, 0.0f, 0.21f, 1.0f, 0.0f, 0.0f), tm);

        if (span.syntheticBold) {
            out += num(span.sizePoints * 0.03) + " w\n";
            out += colorOps(span.color, "RG");
        }

        out += "BT\n/" + resourceNameFor(span) + ' ' + num(span.sizePoints) + " Tf\n";
        out += span.syntheticBold ? "0 Tc 0 Tw 100 Tz 2 Tr 0 Ts\n"
                                  : "0 Tc 0 Tw 100 Tz 0 Tr 0 Ts\n";
        out += colorOps(span.color, "rg");
        out += num(tm.a) + ' ' + num(tm.b) + ' ' + num(tm.c) + ' ' + num(tm.d) + ' '
             + num(tm.e) + ' ' + num(tm.f) + " Tm\n";
        out += '<' + bytes.toHex() + "> Tj\nET\n";

        if (span.underlined && span.width > 0.0) {
            const double y = span.origin.y() + span.sizePoints * 0.12;
            const fz_point a = fz_transform_point(
                fz_make_point(float(span.origin.x()), float(y)), inverse);
            const fz_point b = fz_transform_point(
                fz_make_point(float(span.origin.x() + span.width), float(y)), inverse);
            out += num(span.sizePoints * 0.06) + " w\n";
            out += colorOps(span.color, "RG");
            out += num(a.x) + ' ' + num(a.y) + " m\n" + num(b.x) + ' ' + num(b.y) + " l\nS\n";
        }
    }

    out += "Q\n";
    return out;
}

}

bool PdfEditor::replaceTexts(int pageIndex, const QVector<TextReplacement> &items,
                             int *unsupportedCharacters)
{
    m_error.clear();
    if (unsupportedCharacters)
        *unsupportedCharacters = 0;
    if (!m_doc) {
        m_error = "No document is open.";
        return false;
    }
    if (items.isEmpty()) {
        m_error = "There is no text to replace.";
        return false;
    }
    for (const TextReplacement &item : items) {
        if (!item.eraseArea.isValid() || item.eraseArea.isEmpty()) {
            m_error = "The text area is empty.";
            return false;
        }
    }

    clearSelectionTextCache();

    fz_matrix ctm = fz_identity;
    fz_var(ctm);
    fz_try(m_ctx)
    {
        pdf_obj *pageObject = pdf_lookup_page_obj(m_ctx, m_doc, pageIndex);
        fz_rect box = fz_empty_rect;
        pdf_page_obj_transform(m_ctx, pageObject, &box, &ctm);
    }
    fz_catch(m_ctx)
    {
        m_error = QString::fromUtf8(fz_caught_message(m_ctx));
        return false;
    }
    const fz_matrix inverse = fz_invert_matrix(ctm);

    int unsupported = 0;
    const QByteArray content = buildContent(inverse, items, &unsupported);

    QList<QByteArray> baseFonts;
    QList<QByteArray> fontFiles;
    QList<QByteArray> resourceNames;
    QSet<QString> seen;
    for (const TextReplacement &item : items) {
        for (const TextSpan &span : item.spans) {
            const QByteArray resourceName = resourceNameFor(span);
            const QString seenKey = QString::fromLatin1(resourceName);
            if (seen.contains(seenKey))
                continue;
            seen.insert(seenKey);
            baseFonts.append(span.font.toLatin1());
            fontFiles.append(span.fontFile);
            resourceNames.append(resourceName);
        }
    }

    std::vector<fz_rect> redactRects;
    for (const TextReplacement &item : items) {
        if (!item.removeOriginalText)
            continue;
        const QRectF &area = item.eraseArea;
        const double inset = qMin(area.height() * 0.12, 2.0);
        redactRects.push_back(fz_make_rect(
            float(area.left()), float(area.top() + inset),
            float(area.right()), float(area.bottom() - inset)));
    }

    pdf_page *page = nullptr;
    pdf_annot *annot = nullptr;
    fz_buffer *buffer = nullptr;
    fz_font *font = nullptr;
    pdf_obj *fontRef = nullptr;
    pdf_obj *stream = nullptr;
    pdf_obj *newContents = nullptr;
    fz_buffer *fontBuffer = nullptr;
    fz_var(page);
    fz_var(annot);
    fz_var(buffer);
    fz_var(font);
    fz_var(fontRef);
    fz_var(stream);
    fz_var(newContents);
    fz_var(fontBuffer);

    fz_try(m_ctx)
    {
        if (!redactRects.empty()) {
            page = pdf_load_page(m_ctx, m_doc, pageIndex);
            const fz_rect pageBounds = fz_bound_page(m_ctx, &page->super);
            bool marked = false;
            for (const fz_rect &rect : redactRects) {
                const fz_rect target = fz_intersect_rect(rect, pageBounds);
                if (fz_is_empty_rect(target))
                    continue;

                annot = pdf_create_annot(m_ctx, page, PDF_ANNOT_REDACT);
                pdf_set_annot_rect(m_ctx, annot, target);
                pdf_update_annot(m_ctx, annot);
                pdf_drop_annot(m_ctx, annot);
                annot = nullptr;
                marked = true;
            }

            if (marked) {
                pdf_redact_options options = {};
                options.black_boxes = 0;
                options.image_method = PDF_REDACT_IMAGE_NONE;
                options.line_art = PDF_REDACT_LINE_ART_NONE;
                options.text = PDF_REDACT_TEXT_REMOVE;
                pdf_redact_page(m_ctx, m_doc, page, &options);
            }
            fz_drop_page(m_ctx, &page->super);
            page = nullptr;
        }

        pdf_obj *pageObject = pdf_lookup_page_obj(m_ctx, m_doc, pageIndex);
        pdf_obj *resources = pdf_dict_get_inheritable(m_ctx, pageObject, PDF_NAME(Resources));
        if (!resources)
            resources = pdf_dict_put_dict(m_ctx, pageObject, PDF_NAME(Resources), 4);
        pdf_obj *fontDict = pdf_dict_get(m_ctx, resources, PDF_NAME(Font));
        if (!fontDict)
            fontDict = pdf_dict_put_dict(m_ctx, resources, PDF_NAME(Font), 4);

        for (int i = 0; i < int(baseFonts.size()); ++i) {
            if (pdf_dict_gets(m_ctx, fontDict, resourceNames.at(i).constData()))
                continue;

            if (!fontFiles.at(i).isEmpty()) {
                fz_try(m_ctx)
                {
                    fontBuffer = fz_new_buffer_from_copied_data(
                        m_ctx,
                        reinterpret_cast<const unsigned char *>(fontFiles.at(i).constData()),
                        size_t(fontFiles.at(i).size()));
                    font = fz_new_font_from_buffer(m_ctx, nullptr, fontBuffer, 0, 0);
                    fontRef = pdf_add_simple_font(m_ctx, m_doc, font,
                                                  PDF_SIMPLE_ENCODING_LATIN);
                }
                fz_always(m_ctx)
                {
                    if (fontBuffer) {
                        fz_drop_buffer(m_ctx, fontBuffer);
                        fontBuffer = nullptr;
                    }
                }
                fz_catch(m_ctx)
                {
                    fontRef = nullptr;
                    if (font) {
                        fz_drop_font(m_ctx, font);
                        font = nullptr;
                    }
                }
            }

            if (!fontRef) {
                font = fz_new_base14_font(m_ctx, baseFonts.at(i).constData());
                fontRef = pdf_add_simple_font(m_ctx, m_doc, font, PDF_SIMPLE_ENCODING_LATIN);
            }
            pdf_dict_puts(m_ctx, fontDict, resourceNames.at(i).constData(), fontRef);
            pdf_drop_obj(m_ctx, fontRef);
            fontRef = nullptr;
            fz_drop_font(m_ctx, font);
            font = nullptr;
        }

        buffer = fz_new_buffer_from_copied_data(
            m_ctx, reinterpret_cast<const unsigned char *>(content.constData()),
            size_t(content.size()));
        stream = pdf_add_stream(m_ctx, m_doc, buffer, nullptr, 0);

        pdf_obj *contents = pdf_dict_get(m_ctx, pageObject, PDF_NAME(Contents));
        if (pdf_is_array(m_ctx, contents)) {
            pdf_array_push(m_ctx, contents, stream);
        } else {
            newContents = pdf_new_array(m_ctx, m_doc, 2);
            if (contents)
                pdf_array_push(m_ctx, newContents, contents);
            pdf_array_push(m_ctx, newContents, stream);
            pdf_dict_put(m_ctx, pageObject, PDF_NAME(Contents), newContents);
        }
    }
    fz_always(m_ctx)
    {
        if (fontBuffer)
            fz_drop_buffer(m_ctx, fontBuffer);
        if (newContents)
            pdf_drop_obj(m_ctx, newContents);
        if (stream)
            pdf_drop_obj(m_ctx, stream);
        if (buffer)
            fz_drop_buffer(m_ctx, buffer);
        if (fontRef)
            pdf_drop_obj(m_ctx, fontRef);
        if (font)
            fz_drop_font(m_ctx, font);
        if (annot)
            pdf_drop_annot(m_ctx, annot);
        if (page)
            fz_drop_page(m_ctx, &page->super);
    }
    fz_catch(m_ctx)
    {
        m_error = QString::fromUtf8(fz_caught_message(m_ctx));
        return false;
    }

    if (unsupportedCharacters)
        *unsupportedCharacters = unsupported;
    return true;
}

bool PdfEditor::replaceText(int pageIndex, const QRectF &eraseArea, const QColor &fillColor,
                            bool removeOriginalText, const QVector<TextSpan> &spans,
                            int *unsupportedCharacters)
{
    TextReplacement item;
    item.eraseArea = eraseArea;
    item.fillColor = fillColor;
    item.removeOriginalText = removeOriginalText;
    item.spans = spans;
    return replaceTexts(pageIndex, { item }, unsupportedCharacters);
}