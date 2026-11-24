# Third-Party Notices

This file records third-party software identified in the project's current
development configuration. It is an inventory, not legal advice or a
certification that a particular source or binary distribution satisfies every
license obligation.

## Scope of this inventory

The versions below reflect the maintainer's development setup: Qt 6.8.0 for
MSVC 2022 and MuPDF 1.26.10 and OpenSSL 3.6.5 from vcpkg using the
`x64-windows-static-md` triplet. Tesseract and the libraries it brings in come from the
same vcpkg installation (see the Tesseract section). The vcpkg component versions in the
MuPDF table were read from that local installation.
Other Qt installations, vcpkg baselines, features, or triplets can produce a
different dependency set and must be checked separately.

The source repository does not include Qt, MuPDF, OpenSSL, or Tesseract binaries. The
application links to Qt dynamically and links MuPDF, OpenSSL, and Tesseract statically
when built with the configuration above. A public source repository and a
distributed executable are different distribution cases; these notices alone
do not satisfy all requirements for distributing a binary.

## Qt

- Project: [Qt](https://www.qt.io/)
- Development version and kit: Qt 6.8.0, MSVC 2022, 64-bit.
- CMake component requested by this project: Widgets (with its Qt dependencies).
- The packaging run's `windeployqt` output selected Qt Core, Gui, Svg, and
  Widgets runtime libraries and plugins. It also selected Qt Network and Qt Pdf
  only as dependencies of optional plugins; the application does not use them,
  and `scripts/package.ps1` removes them and those plugins from the package. The
  exact deployed set can vary with the Qt installation and build.
- The maintainer reports using the LGPL-3.0 option. Confirm the selected terms
  and eligibility for the exact installed Qt modules and package before
  distributing the application.
- License texts in this repository: `licenses/LGPL-3.0.txt` and
  `licenses/GPL-3.0.txt`.
- General licensing information:
  [Qt licensing](https://www.qt.io/licensing/).

## MuPDF and vcpkg-built dependencies

MuPDF is provided by [Artifex Software](https://mupdf.com/).
The development build uses MuPDF 1.26.10, statically linked from the vcpkg
`x64-windows-static-md` triplet. The vcpkg-installed copyright and license
notices for MuPDF and the dependencies listed below are kept in
`licenses/vcpkg/`.

| Component | Installed version | License identified in the installed package | Notice file |
|---|---:|---|---|
| MuPDF | 1.26.10 | GNU AGPL v3.0 only | `licenses/vcpkg/libmupdf-copyright.txt`; license text: `licenses/AGPL-3.0.txt` |
| FreeType | 2.14.3 | FreeType License (FTL) or GPL-2.0-or-later | `licenses/vcpkg/freetype-copyright.txt` |
| Gumbo | 0.14.1 | Apache License 2.0 | `licenses/vcpkg/gumbo-copyright.txt` |
| HarfBuzz | 14.5.1 | MIT-family licenses; see the included notice for component-specific terms | `licenses/vcpkg/harfbuzz-copyright.txt` |
| jbig2dec | 0.20 | GNU AGPL v3.0 | `licenses/vcpkg/jbig2dec-copyright.txt`; license text: `licenses/AGPL-3.0.txt` |
| libjpeg-turbo | 3.2.0 | BSD 3-Clause and Independent JPEG Group (IJG) license terms | `licenses/vcpkg/libjpeg-turbo-copyright.txt` |
| OpenJPEG | 2.5.4 | BSD 2-Clause | `licenses/vcpkg/openjpeg-copyright.txt` |
| zlib | 1.3.2 | zlib license | `licenses/vcpkg/zlib-copyright.txt` |
| Brotli | 1.2.0 | MIT | `licenses/vcpkg/brotli-copyright.txt` |
| bzip2 | 1.0.8 | bzip2 license | `licenses/vcpkg/bzip2-copyright.txt` |
| libpng | 1.6.59 | libpng License version 2 | `licenses/vcpkg/libpng-copyright.txt` |
| libspng | 0.7.4 | BSD 2-Clause and libpng License version 2 | `licenses/vcpkg/libspng-copyright.txt` |
| OpenSSL | 3.6.5 | Apache License 2.0 | `licenses/vcpkg/openssl-copyright.txt` |

Some listed libraries are transitive dependencies of other libraries in the
MuPDF build. The table records the installed package set, not a claim that the
application calls each library directly. The accompanying copyright files
contain the applicable notices and, where supplied by vcpkg, license text.

The installed MuPDF port metadata reports AGPL-3.0-only. Static linking is
therefore a material release consideration. Before distributing a binary,
determine and meet the obligations that apply to the exact MuPDF build and
combined application, including any required corresponding source and build
materials. The packaging script does not provide those materials. Obtain legal
advice if you need a determination of how the license applies to this project.

## Tesseract OCR and its dependencies

Reconstruct mode uses [Tesseract](https://github.com/tesseract-ocr/tesseract) to
recognize text on pages that have no text layer of their own (the code is in
`modules/pdf-reconstruction`). Tesseract comes from vcpkg for the same
`x64-windows-static-md` triplet and is linked statically into the executable, like
MuPDF. Its Leptonica image library, and the image and archive libraries that
Leptonica and Tesseract depend on, come with it.

| Component | License | Notice file |
|---|---|---|
| Tesseract OCR | Apache License 2.0 | `licenses/vcpkg/tesseract-copyright.txt` |
| Leptonica | Leptonica license (BSD 2-Clause style) | `licenses/vcpkg/leptonica-copyright.txt` |
| giflib | MIT-style | `licenses/vcpkg/giflib-copyright.txt` |
| libwebp | BSD 3-Clause | `licenses/vcpkg/libwebp-copyright.txt` |
| libtiff | libtiff license (BSD-style) | `licenses/vcpkg/tiff-copyright.txt` |
| libarchive | BSD 2-Clause | `licenses/vcpkg/libarchive-copyright.txt` |
| curl | curl license (MIT-style) | `licenses/vcpkg/curl-copyright.txt` |

The table follows the dependencies that the vcpkg `tesseract` port (Leptonica,
libarchive, curl) and `leptonica` port (giflib, libjpeg-turbo, libpng, libwebp,
OpenJPEG, libtiff, zlib) declare. libjpeg-turbo, libpng, OpenJPEG, and zlib are the
ones already listed in the MuPDF table above. Installed versions are not recorded
here: add them from `vcpkg list` when cutting a release, because the vcpkg checkout
on the CI runner can differ from the maintainer's local installation. The set of
dependencies can also change with the vcpkg baseline, so compare this table with the
`licenses/vcpkg/` folder of the staged release (the release workflow copies the
copyright file of every installed vcpkg package into it) before publishing.

The notice files are the copyright files that vcpkg installs with each package; the
release workflow collects them, so they match the exact build.

### OCR language data

`tessdata/eng.traineddata` is the English language model that Tesseract loads at run
time. It is a data file, not code, and CMake copies the `tessdata` folder next to the
executable when the folder exists (without it, OCR is unavailable and the app still
builds). The Tesseract project publishes its trained data under the Apache License
2.0. Confirm which repository and variant the shipped file comes from (`tessdata`,
`tessdata_fast`, or `tessdata_best`) and its license for that exact file, and keep that
license text with the release.

## Build and packaging tools

The development workflow uses CMake, Visual Studio/MSVC, and vcpkg. The release
installer is built with [Inno Setup](https://jrsoftware.org/isinfo.php), which is
used only as a build tool; its setup stub is embedded in the generated installer.
Review Inno Setup's license (<https://jrsoftware.org/files/is/license.txt>) and
confirm that its terms permit distributing the installer it generates. The package
manager and build tools are not linked into the application by this project.
The vcpkg CMake helper ports and Meson are build-time dependencies in the
installed vcpkg environment; they are not listed above as application runtime
components. Re-check the toolchain's own terms if redistributing any of those
tools.

## Project files and assets

The project's own source code is licensed under the GNU AGPL v3.0; see
`LICENSE`. The application icon artwork (`resources/pdfappicon.png` and
`resources/pdfappicon.ico`) is project artwork supplied by the maintainer and
is not identified here as third-party material.

## Before publishing or releasing

- For a source-only repository, keep this inventory and the referenced license
  texts with the project. This does not determine whether every aspect of a
  source publication complies with every applicable license.
- For a binary release, inspect the staged archive and create a dependency
  inventory for that exact build, including the Qt libraries, plugins, and
  other runtime files actually included. This development snapshot is not a
  substitute for that release-specific inventory.
- Confirm the redistribution terms for the exact Qt modules and dependencies,
  include their applicable notices and license texts, and preserve the
  applicable rights for users of the dynamically linked Qt libraries. The
  packaging script stages the repository's current license materials; verify
  they cover the exact release build.
- Before distributing the statically linked MuPDF build, determine and meet
  the applicable obligations for the exact build, including any required
  corresponding source, build materials, and instructions. The packaging
  script does not generate or stage those materials.
- The release workflow copies the copyright file of every installed vcpkg package, and
  the `tessdata` folder when the repository has one, into the staged folder. Check the
  staged `licenses/vcpkg/` folder against the tables above, and make sure the installer
  script installs `tessdata/` and `licenses/` as well.
- Release assets are `PDFhome-Setup-<version>.exe` (Inno Setup installer) and
  `PDFhome-windows.zip` (portable). Both must contain the same program files and
  the same `LICENSE`, `THIRD_PARTY_NOTICES.md`, and `licenses/` folder. The installer
  must install these notices alongside the program, and the Qt DLLs must remain
  separate files that can be replaced with compatible modified versions.
- The MSIX package (`PDFhome.msix`, built for Microsoft Store submission and not
  attached to the GitHub Release) is staged from the same folder as the installer, so
  it should carry the same program files, `LICENSE`, `THIRD_PARTY_NOTICES.md`, and
  `licenses/` folder. Check this in the built package. The package also declares
  Microsoft's C++ runtime framework package (`Microsoft.VCLibs.140.00.UWPDesktop`) as a
  dependency; the Store installs it, and it is not bundled in the package.
- Microsoft Store listing: the Store applies its own default license terms to an app
  unless the listing supplies the developer's own, and those defaults can conflict with
  the rights the AGPL gives users. Put the project's license terms (the AGPL, see
  `LICENSE`) in the Store listing, and link the project's source repository (the exact
  tagged commit for each release) and the privacy policy there.
- For each release, link the exact tagged source commit in the release notes (or
  attach a source archive) and keep the build instructions in `README.md` accurate
  for that tag, so the corresponding source for the AGPL-licensed components
  (this project, MuPDF, jbig2dec) is available to recipients.
- Recheck this file whenever Qt, vcpkg, MuPDF, OpenSSL, Tesseract, the triplet, or enabled
  features change. Do not rely on this development-build snapshot as a release
  SBOM.