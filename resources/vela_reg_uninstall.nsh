; Mirror of vela_reg_install.nsh - every WriteRegStr in there has a
; DeleteRegKey here so uninstall leaves no orphans behind.

DeleteRegKey HKCR "Applications\Vela.exe\SupportedTypes"
DeleteRegKey HKCR "Applications\Vela.exe\DefaultIcon"
DeleteRegKey HKCR "Applications\Vela.exe\shell"
DeleteRegKey HKCR "Applications\Vela.exe"
DeleteRegKey HKLM "SOFTWARE\Microsoft\Windows\CurrentVersion\App Paths\Vela.exe"

DeleteRegKey HKCR ".mp4\OpenWithList\Vela.exe"
DeleteRegKey HKCR ".mkv\OpenWithList\Vela.exe"
DeleteRegKey HKCR ".avi\OpenWithList\Vela.exe"
DeleteRegKey HKCR ".mov\OpenWithList\Vela.exe"
DeleteRegKey HKCR ".webm\OpenWithList\Vela.exe"
DeleteRegKey HKCR ".wmv\OpenWithList\Vela.exe"
DeleteRegKey HKCR ".flv\OpenWithList\Vela.exe"
DeleteRegKey HKCR ".m4v\OpenWithList\Vela.exe"
DeleteRegKey HKCR ".mpg\OpenWithList\Vela.exe"
DeleteRegKey HKCR ".mpeg\OpenWithList\Vela.exe"
DeleteRegKey HKCR ".ts\OpenWithList\Vela.exe"
DeleteRegKey HKCR ".3gp\OpenWithList\Vela.exe"
DeleteRegKey HKCR ".png\OpenWithList\Vela.exe"
DeleteRegKey HKCR ".jpg\OpenWithList\Vela.exe"
DeleteRegKey HKCR ".jpeg\OpenWithList\Vela.exe"
DeleteRegKey HKCR ".gif\OpenWithList\Vela.exe"
DeleteRegKey HKCR ".bmp\OpenWithList\Vela.exe"
DeleteRegKey HKCR ".webp\OpenWithList\Vela.exe"
DeleteRegKey HKCR ".tif\OpenWithList\Vela.exe"
DeleteRegKey HKCR ".tiff\OpenWithList\Vela.exe"

System::Call 'shell32::SHChangeNotify(i 0x08000000, i 0, i 0, i 0)'
