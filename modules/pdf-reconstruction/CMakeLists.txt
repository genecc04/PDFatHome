find_package(Tesseract CONFIG REQUIRED)

add_library(PdfReconstruction STATIC

    src/ReconstructionEngine.cpp
    src/ParagraphGrouper.cpp
    src/AppearanceEstimator.cpp
    src/AutoReconstruct.cpp
    src/FontClassifier.cpp
    src/RichText.cpp

    include/reconstruction/ReconstructionEngine.h
    include/reconstruction/ReconstructedPage.h
    include/reconstruction/ParagraphGrouper.h
    include/reconstruction/AppearanceEstimator.h
    include/reconstruction/AutoReconstruct.h
    include/reconstruction/FontClassifier.h
    include/reconstruction/RichText.h
)

target_compile_features(PdfReconstruction PUBLIC cxx_std_17)

# Read sources as UTF-8 on MSVC (the default is the system code page).
if(MSVC)
    target_compile_options(PdfReconstruction PRIVATE /utf-8)
endif()

target_include_directories(PdfReconstruction
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# Tesseract stays PRIVATE: no public header includes a Tesseract header
# (ReconstructionEngine hides it behind a pimpl), so the app never needs
# Tesseract's include paths.
target_link_libraries(PdfReconstruction
    PUBLIC
        Qt6::Core
        Qt6::Gui
    PRIVATE
        Tesseract::libtesseract
)