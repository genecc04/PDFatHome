# PDF@home Privacy Policy

**Effective date:** November 23, 2026

PDF@home is an offline PDF reader and editor for Windows. This policy explains what information the application handles and how it is used.

## 1. Information collection and transmission

PDF@home does not operate an account system or a cloud service. Based on the application's current implementation, it does not collect or send users' personal information, PDF documents, document contents, or usage data to the developer or to PDF@home servers.

The application does not include an integrated analytics or advertising service, and it does not include an automatic crash-reporting service. If the application crashes, diagnostic information is written only to a file on your device (see section 3); nothing is sent anywhere.

## 2. PDF documents and local processing

PDF files are opened and processed on the user's device. Reading, searching, annotating, editing, signing, saving, and text recognition (OCR) of scanned pages are performed locally. PDF documents and page images are not uploaded by PDF@home. Text recognition uses a language data file that is installed with the application, so it does not need an internet connection.

The application may create temporary working files while loading, editing, or signing a document. These files are used for application functionality and are managed locally by the application. Users should save documents to a location they choose and should close the application normally to allow temporary-file cleanup to complete.

## 3. Information stored on the device

PDF@home stores certain application data locally, including:

- Recent-file history, which contains paths to recently opened PDF files.
- Window size, position, and layout preferences.
- Electronic signature images that the user chooses to save for reuse.
- Temporary working files and other short-lived data needed to perform document operations.
- A diagnostic log (`pdfhome.log`) that records warnings, errors, and short progress notes to help with troubleshooting. It may include details such as page numbers, temporary file locations, and error messages from the PDF engine. The application starts a new log at launch if the previous one has grown past about 512 KB.
- A crash file (`crash.dmp`). If the application crashes on Windows, it writes a Windows minidump, which is a snapshot of the program's state at the moment it failed, and a note in the log. The next crash replaces the file. Because it is a snapshot of the program's memory state, it may contain fragments of the data the application was handling at that moment, which could include parts of a document that was open.

The log and the crash file are kept in the application's local data folder in your Windows user profile, alongside your saved signature images. This information remains on the device and is not sent to the developer by PDF@home. Users can clear the recent-file list within the application and can manage or delete locally saved signature images using the application's available controls or Windows file-management tools. The log and crash file can be deleted with Windows file-management tools while the application is closed.

## 4. Internet access and external links

An internet connection is not required for PDF@home's core PDF reading and editing features.

PDF@home can recognize web links contained in PDF documents. If the user chooses to open one, the application asks for confirmation and then passes the link to the user's default web browser. The browser may connect to the linked website, which may collect information under its own privacy policy. That browsing activity is outside PDF@home's control.

PDF@home does not itself upload the PDF or its contents when opening an external link.

## 5. Third-party components

PDF@home uses third-party open-source components to provide its functionality, including Qt, MuPDF, Tesseract (for OCR), and OpenSSL. Their notices and applicable license information are provided with the application and in the project's source repository.

The current application implementation does not integrate these components as online analytics, advertising, or document-upload services. This policy does not cover separate software or services, such as the user's web browser, that the user chooses to use independently.

## 6. Children's privacy

PDF@home does not knowingly collect personal information from children or other users. The application does not provide an account or online user-submission service.

## 7. Changes to this policy

This policy may be updated when the application's functionality or data practices change. The latest version will be published in the project's official source repository.

## 8. Contact

For questions about this privacy policy or PDF@home's privacy practices, please use the project's GitHub issue tracker:

https://github.com/genecc04/PDFatHome/issues

Please do not attach private or sensitive PDF documents, or the crash file (`crash.dmp`), to a public issue. If you share the log file, look through it first.