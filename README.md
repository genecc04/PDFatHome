# PDF@home

**Version 1.3.0**

A desktop PDF reader and editor for Windows, built with C++, Qt 6, MuPDF and Tesseract.
MuPDF handles PDF loading, rendering, search and editing; Qt provides the user
interface; Tesseract reads the text on scanned pages (OCR) so it can be edited.
Everything happens on your computer: your files are never uploaded. The app only opens
web links embedded in a PDF in your default browser after you confirm.

## Features

**Reading**
- Open PDFs, including password-protected files; navigate pages, zoom, search, and browse bookmarks
- Select and copy text; customize panels and window layout

**Page editing**
- Rotate, reorder, extract, insert, or delete pages; undo and redo edits
- Save changes or a copy, and create a losslessly optimized PDF
- Redact selected content in a separate copy
- Export pages as PNG or SVG

**Editing existing text**
- Reconstruct mode outlines the text on the current page, using the page's own text or
  OCR (Tesseract) on scanned pages; click a region to change its wording, font, size,
  color, or position
- Edits wait as a preview until you choose Apply reconstruction (or save), which writes
  them into the PDF as real, selectable text and leaves the mode; Undo steps back
  through the waiting edits

**Marking up and filling out**
- Fill PDF forms; add and edit text, highlights, drawings, and images
- Create visual E-signatures by typing, drawing, or uploading; saved signatures are
  listed in the Tools pane, where you click one to place it or right-click to rename or
  delete it (they are stored locally)
- Move, resize, crop, or delete added images and signatures
- Tool options (font and size, pen and highlighter settings, image crop and delete,
  the list of marked redaction areas) are in the Tools pane on the right

**Security**
- Set or remove AES-256 passwords; digitally sign with an RSA certificate
- Review PDF links before opening them in your browser

## Download

Get PDF@home from the [Microsoft Store](https://apps.microsoft.com/detail/9P25R3SQG4VP?hl=en-us&gl=PH&ocid=pdpshare).

Or get either file from the
[Releases page](https://github.com/genecc04/PDFatHome/releases):

- **`PDFhome-Setup-1.3.0.exe`** (installer): run it and follow the prompts. It adds a Start
  menu entry and an uninstaller.
- **`PDFhome-windows.zip`** (portable): unzip it anywhere and run `PDF@home.exe`. No
  installation is needed.

Both contain the same program and license files. Windows SmartScreen may warn about
an unrecognized publisher because the files are not code-signed; choose **More info >
Run anyway** if you trust the download.

**Keep all the files in the portable folder together.** `PDF@home.exe` needs the Qt
libraries that come with it and does not run on its own. OCR of scanned pages
needs the `tessdata` folder next to the exe; without it the app still works, but only
on pages that have their own text. The release packaging also
deploys the MSVC runtime alongside the application. If you build or repackage the app
without those runtime files, install the
[Microsoft Visual C++ Redistributable (x64)](https://learn.microsoft.com/cpp/windows/latest-supported-vc-redist).

Uninstalling removes the program files. Settings, saved e-signatures, and the local
diagnostic log stored in your user profile are not part of the installation. See
[PRIVACY.md](PRIVACY.md) for what the app stores on your device.

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
| Mark redaction areas | Ctrl+Shift+R |
| Add image | Ctrl+I |
| Zoom | Ctrl + mouse wheel |
| Cancel what you are placing | Esc |
| Finish editing a text box | Ctrl+Enter |
| Bold / italic / underline while editing text | Ctrl+B / Ctrl+I / Ctrl+U |
| Nudge text in reconstruct mode (Shift for bigger steps) | Alt + arrow keys |

## Known limitations

- **Some PDF form field types may not be supported.** The app supports built-in text
  fields, checkboxes, radio buttons, combo boxes and list boxes. Fields that are read-only
  cannot be edited.
- **White-out only covers.** The text under it is still in the file, so search and
  copy still find it. Don't use it to hide anything sensitive.
- **Redaction applies only to marked page content in the new copy.** It does not scrub
  metadata, attachments, bookmarks, form values, or unmarked content. The original
  remains unchanged. Inspect the output and verify that sensitive content is gone
  before sharing it.
- **Digital signatures use an RSA certificate in a PFX/P12 file.** The app can create
  a password-protected self-signed certificate for convenience (minimum 12-character
  password), but recipients may see signatures made with it as untrusted until they trust
  that certificate. Keep the `.p12` file and password safe. The app does not provide a
  trusted timestamp or a certificate trust assessment. Trust is determined by the PDF
  reader and the recipient's certificate store. Editing or rewriting a signed PDF can
  invalidate its signature.
- Use **View > Verify digital signatures** to check signature integrity and detect
  changes since signing. PDF@home does not verify certificate trust chains; use a trusted
  PDF reader for that assessment.
- Text boxes you add use only the standard PDF fonts Helvetica, Times, and Courier for
  cross-viewer compatibility. Text written by reconstruct mode uses the PDF's own font
  when it is installed on your computer as a plain TrueType font that allows embedding
  (Windows only); otherwise it falls back to the matching standard font.
- Reconstruct mode writes text in the Windows-1252 character set only. Any other
  character (Cyrillic, Greek, CJK...) is replaced with "?".
- On a scanned page, reconstruct mode covers the old text with a flat color estimated
  from the scan, which can look patchy over photos or gradients. On pages with their
  own text, only the old letters are removed.
- OCR uses English language data only, and its accuracy depends on the scan.
- Scanned PDFs still have no text to select, copy, or search. OCR runs only in
  reconstruct mode, to find regions you can edit; it does not make the rest of the page
  searchable.
- Inserting pages from a password-protected PDF is not supported.
- Bookmarks are not updated after you delete or reorder pages.
- Reordered or extracted files lose their accessibility tagging.
- Built and tested on Windows with MSVC only.

## Building from source

Requirements: CMake 3.21+, a C++17 compiler (developed with MSVC), Qt 6 Widgets,
and [vcpkg](https://github.com/microsoft/vcpkg).
The development build was tested with Qt 6.8.0 for MSVC 2022 and MuPDF 1.26.10
and Tesseract from vcpkg. QMake reports version 3.1 with that Qt installation; this project uses
CMake to configure and build.

Install MuPDF, OpenSSL and Tesseract with vcpkg (Tesseract brings in Leptonica and the
image libraries it needs):

```
vcpkg install libmupdf openssl tesseract --triplet=x64-windows-static-md --host-triplet=x64-windows-static-md
```

For OCR, put the language data in a `tessdata` folder at the top of the repository
(`eng.traineddata` from the
[tessdata_fast](https://github.com/tesseract-ocr/tessdata_fast) repository, or from the
full tessdata one). CMake copies that folder next to the program after every build.
Without it the build still succeeds (CMake prints a warning) and reconstruct mode still
works on pages that have their own text, but scanned pages cannot be recognized.

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
- `modules/pdf-reconstruction`: finds the text on a page (its own text layer, or OCR
  with Tesseract), groups it into paragraphs, and lays out edited text; built as a
  static library
- `resources`: application icons and Qt/Windows resource files
- `tessdata`: OCR language data, copied next to the program after each build
- `scripts`: development and packaging scripts

### Packaging a release

Releases are built by [GitHub Actions](.github/workflows/ci.yml). Pushing a tag such as
`v1.3.0` builds in Release mode, stages the program with the Qt and MSVC runtimes, the
OCR language data, the README, and the license materials (including the copyright file
of every vcpkg library in the build), and publishes `PDFhome-Setup-1.3.0.exe` (an
[Inno Setup](https://jrsoftware.org/isinfo.php) installer, see
[installer/PDFhome.iss](installer/PDFhome.iss)) and `PDFhome-windows.zip` (portable) on
the Releases page. Keep the tag and the `VERSION` in `CMakeLists.txt` in sync; the
version shown in **Help > About** comes from `CMakeLists.txt`. The workflow also builds
`PDFhome.msix` and keeps it as a workflow artifact for upload to Partner Center; it is
not attached to the Release.

To build a ZIP locally instead:

```
powershell -ExecutionPolicy Bypass -File .\scripts\package.ps1
```

This builds in Release mode and creates `dist/PDF@home-windows.zip`. If needed, pass
`-QtRoot` and `-BuildDir` to match your machine.

Both packages are runtime archives, not complete license-compliance packages: nothing
creates the source or build materials that may be required for the statically linked
MuPDF build. Review [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) and complete its
release checklist before publishing. The installer must install the same `LICENSE`,
`THIRD_PARTY_NOTICES.md`, and `licenses` folder as the ZIP, and must leave the Qt DLLs
as separate files. Each release should link to the exact tagged source commit, so the
corresponding source for the AGPL-licensed components is available.

## Reporting problems

Found a bug? Please open an
[issue](https://github.com/genecc04/PDFatHome/issues) and say which
Windows version you use and what you were doing. If you can share a PDF that shows the
problem, that helps a lot.

## License

This project is licensed under the **GNU Affero General Public License v3.0**.
See [LICENSE](LICENSE). Copyright (C) 2026 genecc04.

It uses [Qt](https://www.qt.io) (LGPL-3.0), [MuPDF](https://mupdf.com)
(AGPL-3.0, Artifex Software, Inc.), and
[Tesseract](https://github.com/tesseract-ocr/tesseract) (Apache-2.0), among other
libraries. The known dependency
inventory and available license texts are documented in
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) and the [licenses](licenses/)
folder.