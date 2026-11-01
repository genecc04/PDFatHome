#pragma once
#include <QColor>
#include <QString>
#include <QList>
#include <QRectF>
#include <QPointF>
#include <QVector>
#include <QByteArray>
#include <QStringList>
#include <functional>

struct fz_context;
struct pdf_document;

class PdfEditor
{
public:
    enum class FormFieldType { None, Text, CheckBox, RadioButton, ComboBox, ListBox };

    struct FormField {
        FormFieldType type = FormFieldType::None;
        QRectF bounds;
        QString name;
        QString label;
        QString value;
        QStringList options;
        QStringList optionValues;
        bool checked = false;
        bool readOnly = false;
        bool multiline = false;
        bool password = false;
        bool editableChoice = false;
        int widgetIndex = -1;
    };

    struct InkStroke {
        QVector<QPointF> points;
        double widthPoints = 1.0;
    };

    PdfEditor();
    ~PdfEditor();

    PdfEditor(const PdfEditor &) = delete;
    PdfEditor &operator=(const PdfEditor &) = delete;
    bool addTextBox(int pageIndex, const QRectF &box, const QString &text,
                const QString &fontName, double fontSize,
                float red, float green, float blue);
    bool textAnnotationAt(int pageIndex, const QPointF &point,
                          int *annotationId, QRectF *bounds, QString *text,
                          QString *fontName, double *fontSize, QColor *color);
    bool updateTextAnnotation(int pageIndex, int annotationId,
                              const QString &oldText, const QString &newText);
    bool setTextAnnotationBounds(int pageIndex, int annotationId, const QString &text,
                                 const QRectF &bounds);
    bool deleteTextAnnotation(int pageIndex, int annotationId, const QString &text);

    bool open(const QString &path, const QString &password = QString());
    int pageCount();
    enum class Encryption { Keep, Remove, Aes256 };

    bool saveAs(const QString &path, bool removeUnused = false,
                Encryption encryption = Encryption::Keep,
                const QString &password = QString());
    bool keepOnlyPages(int firstIndex, int lastIndex);
    bool rotatePage(int pageIndex, int deltaDegrees);
    bool movePage(int fromIndex, int toIndex);
    bool insertPagesFrom(const QString &sourcePath, int insertAt);
    bool deletePage(int pageIndex);
    bool highlightText(const QString &text, QList<int> *changedPages = nullptr,
                       const std::function<bool(int, int)> &progress = {});
    bool bakeAnnotations();
    bool hasAnnotations();
    bool addInkStroke(int pageIndex, const QVector<QPointF> &points,
                    float red, float green, float blue, double width, double opacity);
    bool inkStrokesOnPage(int pageIndex, QVector<InkStroke> *strokes);
    bool eraseInk(int pageIndex, const QVector<QPointF> &path, double radius,
                  int *annotationsRemoved = nullptr);
    bool addImageStamp(int pageIndex, const QRectF &box, const QByteArray &imageData);
    bool formFieldsOnPage(int pageIndex, QVector<FormField> *fields);
    bool formFieldAt(int pageIndex, const QPointF &point, FormField *field);
    bool setFormFieldValue(int pageIndex, int widgetIndex, const QString &value);
    void close();

    QString lastError() const { return m_error; }

private:
    fz_context *m_ctx = nullptr;
    pdf_document *m_doc = nullptr;
    QString m_error;
};