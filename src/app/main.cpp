#include <QApplication>
#include <QIcon>
#include "MainWindow.h"

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    app.setOrganizationName("PdfApp");
    app.setApplicationName("PdfApp");
    app.setApplicationDisplayName("PDF@home");
    app.setApplicationVersion(QStringLiteral(APP_VERSION));
    app.setWindowIcon(QIcon(QStringLiteral(":/icons/pdfappicon.png")));

    MainWindow window;
    window.show();

    const QStringList args = app.arguments();
    if (args.size() > 1)
        window.openPath(args.at(1));

    return app.exec();
}