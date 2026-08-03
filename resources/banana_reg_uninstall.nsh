; Mirror of banana_reg_install.nsh - every WriteRegStr in there has a
; DeleteRegKey here so uninstall leaves no orphans behind.

DeleteRegKey HKCR "Applications\BananaPlayer.exe\SupportedTypes"
DeleteRegKey HKCR "Applications\BananaPlayer.exe\DefaultIcon"
DeleteRegKey HKCR "Applications\BananaPlayer.exe\shell"
DeleteRegKey HKCR "Applications\BananaPlayer.exe"
DeleteRegKey HKLM "SOFTWARE\Microsoft\Windows\CurrentVersion\App Paths\BananaPlayer.exe"

DeleteRegKey HKCR ".mp4\OpenWithList\BananaPlayer.exe"
DeleteRegKey HKCR ".mkv\OpenWithList\BananaPlayer.exe"
DeleteRegKey HKCR ".avi\OpenWithList\BananaPlayer.exe"
DeleteRegKey HKCR ".mov\OpenWithList\BananaPlayer.exe"
DeleteRegKey HKCR ".webm\OpenWithList\BananaPlayer.exe"
DeleteRegKey HKCR ".wmv\OpenWithList\BananaPlayer.exe"
DeleteRegKey HKCR ".flv\OpenWithList\BananaPlayer.exe"
DeleteRegKey HKCR ".m4v\OpenWithList\BananaPlayer.exe"
DeleteRegKey HKCR ".mpg\OpenWithList\BananaPlayer.exe"
DeleteRegKey HKCR ".mpeg\OpenWithList\BananaPlayer.exe"
DeleteRegKey HKCR ".ts\OpenWithList\BananaPlayer.exe"
DeleteRegKey HKCR ".3gp\OpenWithList\BananaPlayer.exe"
DeleteRegKey HKCR ".png\OpenWithList\BananaPlayer.exe"
DeleteRegKey HKCR ".jpg\OpenWithList\BananaPlayer.exe"
DeleteRegKey HKCR ".jpeg\OpenWithList\BananaPlayer.exe"
DeleteRegKey HKCR ".gif\OpenWithList\BananaPlayer.exe"
DeleteRegKey HKCR ".bmp\OpenWithList\BananaPlayer.exe"
DeleteRegKey HKCR ".webp\OpenWithList\BananaPlayer.exe"
DeleteRegKey HKCR ".tif\OpenWithList\BananaPlayer.exe"
DeleteRegKey HKCR ".tiff\OpenWithList\BananaPlayer.exe"

System::Call 'shell32::SHChangeNotify(i 0x08000000, i 0, i 0, i 0)'
