; Inno Setup script for PDF@home.
; CI compiles it with:
;   ISCC /DAppVersion=1.0 "/DAppExeName=PDF@home.exe" installer\PDFhome.iss
; Paths are relative to this file. The staged app folder (exe + Qt DLLs from
; windeployqt) is expected at ..\dist\PDFhome, and the installer is written to ..\output.

#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif
#ifndef AppExeName
  #define AppExeName "PDF@home.exe"
#endif
#define AppName "PDF@home"
#define StageDir "..\dist\PDFhome"

[Setup]
; AppId identifies the app for upgrades/uninstall. Keep this GUID the same in every release.
AppId={{4C9A1E52-7B3D-4F86-A0E5-9D2B6C813F47}
AppName={#AppName}
AppVersion={#AppVersion}
AppPublisher=genecc04
AppPublisherURL=https://github.com/genecc04/offline-pdf-reader-editor
AppSupportURL=https://github.com/genecc04/offline-pdf-reader-editor/issues
DefaultDirName={autopf}\PDFhome
DisableProgramGroupPage=yes
SetupIconFile=..\resources\pdfappicon.ico
UninstallDisplayIcon={app}\{#AppExeName}
LicenseFile=..\LICENSE
OutputDir=..\output
OutputBaseFilename=PDFhome-Setup-{#AppVersion}
Compression=lzma2/max
SolidCompression=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=admin
WizardStyle=modern

[Tasks]
Name: "desktopicon"; Description: "Create a &desktop shortcut"; GroupDescription: "Additional shortcuts:"; Flags: unchecked

[Files]
; Everything windeployqt produced goes into the install folder...
Source: "{#StageDir}\*"; DestDir: "{app}"; Excludes: "vc_redist.x64.exe"; Flags: ignoreversion recursesubdirs createallsubdirs
; ...except the Visual C++ runtime installer, which is unpacked to a temp folder, run once, then deleted.
Source: "{#StageDir}\vc_redist.x64.exe"; DestDir: "{tmp}"; Flags: deleteafterinstall skipifsourcedoesntexist

[Icons]
Name: "{autoprograms}\{#AppName}"; Filename: "{app}\{#AppExeName}"
Name: "{autodesktop}\{#AppName}"; Filename: "{app}\{#AppExeName}"; Tasks: desktopicon

[Run]
Filename: "{tmp}\vc_redist.x64.exe"; Parameters: "/install /quiet /norestart"; StatusMsg: "Installing the Microsoft Visual C++ runtime..."; Flags: waituntilterminated skipifdoesntexist
Filename: "{app}\{#AppExeName}"; Description: "Launch {#AppName}"; Flags: nowait postinstall skipifsilent
