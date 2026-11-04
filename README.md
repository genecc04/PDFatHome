# PDF@home

**Version 1.0**

A desktop PDF reader and editor for Windows, built with C++, Qt 6 and MuPDF.
MuPDF handles PDF loading, rendering, search and editing; Qt provides the user
interface.
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

**Marking up and filling out**
- Fill PDF forms; add and edit text, highlights, drawings, and images
- Create visual E-signatures by typing, drawing, or uploading; save and manage them locally
- Move, resize, crop, or delete added images and signatures

**Security**
- Set or remove AES-256 passwords; digitally sign with an RSA certificate
- Review PDF links before opening them in your browser

## Download

Get either file from the
[Releases page](https://github.com/genecc04/PDFatHome/releases):

- **`PDFhome-Setup-1.0.exe`** (installer): run it and follow the prompts. It adds a Start
  menu entry and an uninstaller.
- **`PDFhome-windows.zip`** (portable): unzip it anywhere and run `PDF@home.exe`. No
  installation is needed.

Both contain the same program and license files. Windows SmartScreen may warn about
an unrecognized publisher because the files are not code-signed; choose **More info >
Run anyway** if you trust the download.

**Keep all the files in the portable folder together.** `PDF@home.exe` needs the Qt
libraries that come with it and does not run on its own. The release packaging also
deploys the MSVC runtime alongside the application. If you build or repackage the app
without those runtime files, install the
[Microsoft Visual C++ Redistributable (x64)](https://learn.microsoft.com/cpp/windows/latest-supported-vc-redist).

Uninstalling removes the program files. Settings and saved e-signatures stored in your
user profile are not part of the installation.

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

Install MuPDF and OpenSSL with vcpkg:

```
vcpkg install libmupdf openssl --triplet=x64-windows-static-md --host-triplet=x64-windows-static-md
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

Releases are built by [GitHub Actions](.github/workflows/ci.yml). Pushing a tag such as
`v1.0` builds in Release mode, stages the program with the Qt and MSVC runtimes, the
README, and the license materials, and publishes `PDFhome-Setup-1.0.exe` (an
[Inno Setup](https://jrsoftware.org/isinfo.php) installer, see
[installer/PDFhome.iss](installer/PDFhome.iss)) and `PDFhome-windows.zip` (portable) on
the Releases page. Keep the tag and the `VERSION` in `CMakeLists.txt` in sync; the
version shown in **Help > About** comes from `CMakeLists.txt`.

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

It uses [Qt](https://www.qt.io) (LGPL-3.0) and [MuPDF](https://mupdf.com)
(AGPL-3.0, Artifex Software, Inc.), among other libraries. The known dependency
inventory and available license texts are documented in
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) and the [licenses](licenses/)
folder.