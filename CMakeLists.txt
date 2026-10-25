cmake_minimum_required(VERSION 3.21)
project(PdfApp LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets Pdf PdfWidgets)
find_package(unofficial-libmupdf CONFIG REQUIRED)

add_executable(PdfApp WIN32
    src/app/main.cpp
    src/app/MainWindow.cpp
    src/app/MainWindow.h
    src/app/MainWindowDocument.cpp
    src/app/MainWindowEditing.cpp
    src/app/MainWindowUi.cpp
    src/core/EditHistory.cpp
    src/core/EditHistory.h
    src/pdf/PdfEditor.cpp
    src/pdf/PdfEditor.h
    src/ui/BookmarkPane.cpp
    src/ui/BookmarkPane.h
    src/ui/ColorMenuButton.cpp
    src/ui/ColorMenuButton.h
    src/ui/PageRangeDialog.cpp
    src/ui/PageRangeDialog.h
    src/ui/PageView.cpp
    src/ui/PageView.h
    src/ui/SearchPane.cpp
    src/ui/SearchPane.h
    src/ui/TextRenderer.cpp
    src/ui/TextRenderer.h
    src/ui/ThumbnailModel.cpp
    src/ui/ThumbnailModel.h
    src/ui/ThumbnailPane.cpp
    src/ui/ThumbnailPane.h
    resources/app_resources.qrc
    resources/pdfappicon.rc
    src/ui/RenderDocumentCache.h
)

target_include_directories(PdfApp PRIVATE
    src/app
    src/core
    src/pdf
    src/ui
)

set_target_properties(PdfApp PROPERTIES OUTPUT_NAME "PDF@home")

target_link_libraries(PdfApp PRIVATE
    Qt6::Widgets Qt6::Pdf Qt6::PdfWidgets
    unofficial::libmupdf::libmupdf
)
