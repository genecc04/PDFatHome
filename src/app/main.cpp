#include <QApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QIcon>
#include <QMutex>
#include <QMutexLocker>
#include <QStandardPaths>
#include <QTextStream>
#include "MainWindow.h"

#ifdef Q_OS_WIN
#include <qt_windows.h>
#include <dbghelp.h>
#include <csignal>
#pragma comment(lib, "Dbghelp.lib")
#endif

namespace {

QString g_logPath;
QMutex g_logMutex;
QtMessageHandler g_previousHandler = nullptr;

void appendLog(const QString &line)
{
    QFile file(g_logPath);
    if (file.open(QIODevice::Append | QIODevice::Text)) {
        QTextStream(&file) << QDateTime::currentDateTime().toString(Qt::ISODateWithMs)
                           << ' ' << line << '\n';
    }
}

void logMessage(QtMsgType type, const QMessageLogContext &context, const QString &message)
{
    if (type != QtDebugMsg) {
        QMutexLocker locker(&g_logMutex);
        appendLog(message);
    }
#ifdef Q_OS_WIN
    OutputDebugStringW(reinterpret_cast<const wchar_t *>((message + QLatin1Char('\n')).utf16()));
#endif
    if (g_previousHandler)
        g_previousHandler(type, context, message);
}

#ifdef Q_OS_WIN
wchar_t g_dumpPath[MAX_PATH] = {};

void writeDump(EXCEPTION_POINTERS *exception)
{
    HANDLE file = CreateFileW(g_dumpPath, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return;

    MINIDUMP_EXCEPTION_INFORMATION info = {};
    info.ThreadId = GetCurrentThreadId();
    info.ExceptionPointers = exception;
    info.ClientPointers = FALSE;

    MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file, MiniDumpNormal,
                      exception ? &info : nullptr, nullptr, nullptr);
    CloseHandle(file);
}

LONG WINAPI onUnhandledException(EXCEPTION_POINTERS *exception)
{
    appendLog(QStringLiteral("CRASH: exception 0x%1 at address 0x%2")
                  .arg(quint32(exception->ExceptionRecord->ExceptionCode), 8, 16, QLatin1Char('0'))
                  .arg(quintptr(exception->ExceptionRecord->ExceptionAddress), 0, 16));
    writeDump(exception);
    return EXCEPTION_CONTINUE_SEARCH;
}

void onAbort(int)
{
    appendLog(QStringLiteral("CRASH: abort() called (failed assertion, qFatal or "
                             "uncaught C++ exception)"));
    writeDump(nullptr);
}
#endif

void installCrashRecording()
{
    const QString folder = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    QDir().mkpath(folder);

    g_logPath = QDir(folder).filePath(QStringLiteral("pdfhome.log"));
    if (QFileInfo(g_logPath).size() > 512 * 1024)
        QFile::remove(g_logPath);
    appendLog(QStringLiteral("---- started ----"));

    g_previousHandler = qInstallMessageHandler(logMessage);

#ifdef Q_OS_WIN
    const QString dumpPath = QDir::toNativeSeparators(QDir(folder).filePath(QStringLiteral("crash.dmp")));
    if (dumpPath.size() < MAX_PATH) {
        dumpPath.toWCharArray(g_dumpPath);
        SetUnhandledExceptionFilter(onUnhandledException);
        std::signal(SIGABRT, onAbort);
    }
#endif
}

}

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    app.setOrganizationName("PdfApp");
    app.setApplicationName("PdfApp");
    app.setApplicationDisplayName("PDF@home");
    app.setApplicationVersion(QStringLiteral(APP_VERSION));
    app.setWindowIcon(QIcon(QStringLiteral(":/icons/pdfappicon.png")));

    installCrashRecording();

    MainWindow window;
    window.show();

    const QStringList args = app.arguments();
    if (args.size() > 1)
        window.openPath(args.at(1));

    return app.exec();
}