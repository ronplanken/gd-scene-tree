#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif
#ifndef SourceDir
  #define SourceDir "..\release\RelWithDebInfo\gd-scene-tree"
#endif
#ifndef OutputDir
  #define OutputDir "..\release"
#endif

[Setup]
AppName=GD Scene Tree
AppVersion={#AppVersion}
AppVerName=GD Scene Tree {#AppVersion}
AppPublisher=Ron Planken
AppPublisherURL=https://github.com/ronplanken/gd-scene-tree
AppSupportURL=https://github.com/ronplanken/gd-scene-tree/issues
DefaultDirName={commonappdata}\obs-studio\plugins\gd-scene-tree
DisableDirPage=yes
DisableProgramGroupPage=yes
OutputDir={#OutputDir}
OutputBaseFilename=gd-scene-tree-{#AppVersion}-windows-x64-installer
Compression=lzma2
SolidCompression=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=admin
UninstallDisplayName=GD Scene Tree
UninstallFilesDir={app}\uninstall
WizardStyle=modern

[Files]
Source: "{#SourceDir}\bin\64bit\*"; DestDir: "{app}\bin\64bit"; Flags: ignoreversion recursesubdirs
Source: "{#SourceDir}\data\*"; DestDir: "{app}\data"; Flags: ignoreversion recursesubdirs

[Messages]
WelcomeLabel2=This installs [name/ver] into the OBS Studio plugin folder for all users.%n%nClose OBS Studio before continuing.
