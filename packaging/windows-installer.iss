#define AppName "AFAR RX Calibration Studio"
#define AppVersion "0.1.0"
#define PackageDir "..\dist\AfarRxCalibrationStudio-0.1.0-win64"

[Setup]
AppId={{3B7BA10E-39E7-4C87-A150-2B66EB7A75A1}
AppName={#AppName}
AppVersion={#AppVersion}
AppPublisher=AFAR Calibration Project
DefaultDirName={localappdata}\Programs\AfarRxCalibrationStudio
DefaultGroupName={#AppName}
OutputDir=..\dist
OutputBaseFilename=AfarRxCalibrationStudio-{#AppVersion}-setup
Compression=lzma2
SolidCompression=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
UninstallDisplayIcon={app}\AfarRxCalibrationStudio.exe
WizardStyle=modern

[Files]
Source: "{#PackageDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\{#AppName}"; Filename: "{app}\AfarRxCalibrationStudio.exe"
Name: "{autodesktop}\{#AppName}"; Filename: "{app}\AfarRxCalibrationStudio.exe"; Tasks: desktopicon

[Tasks]
Name: "desktopicon"; Description: "Создать ярлык на рабочем столе"; GroupDescription: "Дополнительные значки:"

[Run]
Filename: "{app}\AfarRxCalibrationStudio.exe"; Description: "Запустить {#AppName}"; Flags: nowait postinstall skipifsilent
