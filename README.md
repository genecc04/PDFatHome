# PDF@home

A desktop PDF reader and editor for Windows, built with C++, Qt 6 and MuPDF.
MuPDF handles PDF loading, rendering, search and editing; Qt provides the user
interface.
Everything happens on your computer: your files are never uploaded. The app only opens
web links embedded in a PDF in your default browser after you confirm.

## Features

**Reading**
- Open PDFs, including password-protected ones. Drag a file onto the window, use
  File > Open Recent, or open one from the command line
- Page navigation and zoom: zoom in and out, fit width, fit page, Ctrl + mouse wheel.
- Page thumbnails, drawn on demand so large files open quickly
- Full-text search with highlighted results
- Bookmarks / table of contents
- Select text with the mouse and copy it, across pages
- Remembers your window size and panel layout

**Page editing**
- Rotate or delete pages. Select several thumbnails with Shift + click or Ctrl + click
- Reorder pages by dragging thumbnails
- Extract a page range into a new PDF
- Insert pages from another PDF (this also merges documents)
- Undo and redo (up to 20 steps)
- Save in place (Ctrl+S) or save a copy (Save As). Saving replaces the file safely,
  so a failed save never costs you the original
- Export selected page ranges as 150-DPI PNG images or SVG files; multi-page
  exports are numbered

**Marking up and filling out**
- Fill out forms: type text into the blanks, draw check marks, and add your signature
- Add editable PDF text annotations with size and color controls.
- Draw with a pen, a highlighter or white-out, with adjustable size, color and opacity.
- Highlight every occurrence of a word or phrase
- Add images and signatures. Pick a file and click the page. Use Crop in the image
  toolbar and drag a corner handle inward to crop. Newly added images can be selected,
  moved, proportionally resized from a corner, or deleted with Delete.
  Select Cancel or press Esc to stop placing the image

**Security**
- Add, change or remove a password (AES-256)
- Preview website link URLs in the status bar; open links only after confirming

## Download

Get `PDF@home-windows.zip` from the
[Releases page](https://github.com/genecc04/offline-pdf-reader-editor/releases),
unzip it anywhere, and run `PDF@home.exe`. No installer is needed.

**Keep all the files in the folder together.** `PDF@home.exe` needs the Qt libraries that
come with it and does not run on its own.

It also needs the
[Microsoft Visual C++ Redistributable (x64)](https://learn.microsoft.com/cpp/windows/latest-supported-vc-redist),
which most computers already have.

## Keyboard shortcuts

| Action | Shortcut |
|---|---|
| Open / Save / Save As | Ctrl+O / Ctrl+S / Ctrl+Shift+S |
| Undo / Redo | Ctrl+Z / Ctrl+Y |
| Find | Ctrl+F |
| Show or hide the Pages panel / Bookmarks panel | F4 / Ctrl+B |
| Copy selected text / Select all text | Ctrl+C / Ctrl+A |
| Rotate left / right | Ctrl+L / Ctrl+R |
| Highlight text | Ctrl+H |
| Add text | Ctrl+T |
| Draw | Ctrl+D |
| Add image | Ctrl+I |
| Zoom | Ctrl + mouse wheel |
| Cancel what you are placing | Esc |

## Known limitations

- **Some PDF form field types may not be supported.** The app supports built-in text
  fields, checkboxes, radio buttons, combo boxes and list boxes. Fields that are read-only
  cannot be edited.
- **White-out only covers.** The text under it is still in the file, so search and
  copy still find it. Don't use it to hide anything sensitive.
- Added text uses only the standard PDF fonts Helvetica, Times, and Courier for
  cross-viewer compatibility.
- Scanned PDFs have no text to select or search (there is no OCR).
- Inserting pages from a password-protected PDF is not supported.
- Bookmarks are not updated after you delete or reorder pages.
- Reordered or extracted files lose their accessibility tagging.
- Built and tested on Windows with MSVC only.

## Building from source

Requirements: CMake 3.21+, a C++17 compiler (developed with MSVC), Qt 6 Widgets,
and [vcpkg](https://github.com/microsoft/vcpkg).
The development build was tested with Qt 6.8.0 for MSVC 2022 and MuPDF 1.26.10
from vcpkg. QMake reports version 3.1 with that Qt installation; this project uses
CMake to configure and build.

Install MuPDF with vcpkg:

```
vcpkg install libmupdf:x64-windows-static-md --host-triplet=x64-windows-static-md
```

Then configure and build, adjusting the paths to your machine:

```
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake -DVCPKG_TARGET_TRIPLET=x64-windows-static-md -DCMAKE_PREFIX_PATH="C:/Qt/6.8.0/msvc2022_64;C:/vcpkg/installed/x64-windows-static-md"
cmake --build build --config Debug
```

### Project layout

- `src/app`: application entry point and main-window workflows
- `src/pdf`: PDF editing backend
- `src/ui`: document view, panels, dialogs, and UI helpers
- `src/core`: shared application logic such as edit history
- `resources`: application icons and Qt/Windows resource files
- `scripts`: development and packaging scripts

### Packaging a release

```
powershell -ExecutionPolicy Bypass -File .\scripts\package.ps1
```

This builds in Release mode and creates `dist/PDF@home-windows.zip` with the program,
the dynamically linked Qt runtime, and the repository's license materials. If needed,
pass `-QtRoot` and `-BuildDir` to match your machine. The script does not create the
source materials required for distributing the statically linked MuPDF build. Review
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) and complete those release-specific
steps before distributing the archive.

## Reporting problems

Found a bug? Please open an
[issue](https://github.com/genecc04/offline-pdf-reader-editor/issues) and say which
Windows version you use and what you were doing. If you can share a PDF that shows the
problem, that helps a lot.

## License

This project is licensed under the **GNU Affero General Public License v3.0**.
See [LICENSE](LICENSE). Copyright (C) 2026 genecc04.

It uses [Qt](https://www.qt.io) (LGPL-3.0) and [MuPDF](https://mupdf.com)
(AGPL-3.0, Artifex Software, Inc.), among other libraries. The known dependency
inventory and available license texts are documented in
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) and the [licenses](licenses/)
folder.