#pragma once

#include <QString>

// Where a PDF lives on disk and the password needed to open it. Passed as one
// value to everything that opens its own copy of the document.
struct DocumentSource
{
    QString path;
    QString password;

    bool isEmpty() const { return path.isEmpty(); }
};
