; -----------------------------------------------------------------------------
; File association + "Open With" registration for Vela.
;
; Included from CPACK_NSIS_EXTRA_INSTALL_COMMANDS via `!include` so the raw
; NSIS text bypasses the CPackConfig.cmake string round-trip (which strips
; the literal quotes around $INSTDIR\bin\Vela.exe and breaks shell\open\command).
;
; Quoting reminder: inside NSIS "..." strings, `\` is literal; `$\"`
; is a literal double quote. Inside '...' strings, `\` is also literal,
; and inner double quotes are literal too - which is why the two lines
; that need embedded quotes (DefaultIcon and shell\open\command) use
; single-quote wrapping.
; -----------------------------------------------------------------------------

WriteRegStr HKCR "Applications\Vela.exe" "FriendlyAppName" "Banana Player"
WriteRegStr HKCR "Applications\Vela.exe" "ApplicationName" "Banana Player"
WriteRegStr HKCR "Applications\Vela.exe" "ApplicationCompany" "Banana Player"
WriteRegStr HKCR "Applications\Vela.exe" "ApplicationDescription" "Peel and play - lightweight, keyboard-first video playback"
WriteRegStr HKCR "Applications\Vela.exe\DefaultIcon" "" '"$INSTDIR\bin\Vela.exe",0'
WriteRegStr HKCR "Applications\Vela.exe\shell\open\command" "" '"$INSTDIR\bin\Vela.exe" "%1"'
WriteRegStr HKCR "Applications\Vela.exe\shell\open\FriendlyAppName" "" "Banana Player"
WriteRegStr HKLM "SOFTWARE\Microsoft\Windows\CurrentVersion\App Paths\Vela.exe" "" "$INSTDIR\bin\Vela.exe"
WriteRegStr HKLM "SOFTWARE\Microsoft\Windows\CurrentVersion\App Paths\Vela.exe" "Path" "$INSTDIR\bin"

WriteRegStr HKCR ".mp4\OpenWithList\Vela.exe" "" ""
WriteRegStr HKCR "Applications\Vela.exe\SupportedTypes" ".mp4" ""
WriteRegStr HKCR ".mkv\OpenWithList\Vela.exe" "" ""
WriteRegStr HKCR "Applications\Vela.exe\SupportedTypes" ".mkv" ""
WriteRegStr HKCR ".avi\OpenWithList\Vela.exe" "" ""
WriteRegStr HKCR "Applications\Vela.exe\SupportedTypes" ".avi" ""
WriteRegStr HKCR ".mov\OpenWithList\Vela.exe" "" ""
WriteRegStr HKCR "Applications\Vela.exe\SupportedTypes" ".mov" ""
WriteRegStr HKCR ".webm\OpenWithList\Vela.exe" "" ""
WriteRegStr HKCR "Applications\Vela.exe\SupportedTypes" ".webm" ""
WriteRegStr HKCR ".wmv\OpenWithList\Vela.exe" "" ""
WriteRegStr HKCR "Applications\Vela.exe\SupportedTypes" ".wmv" ""
WriteRegStr HKCR ".flv\OpenWithList\Vela.exe" "" ""
WriteRegStr HKCR "Applications\Vela.exe\SupportedTypes" ".flv" ""
WriteRegStr HKCR ".m4v\OpenWithList\Vela.exe" "" ""
WriteRegStr HKCR "Applications\Vela.exe\SupportedTypes" ".m4v" ""
WriteRegStr HKCR ".mpg\OpenWithList\Vela.exe" "" ""
WriteRegStr HKCR "Applications\Vela.exe\SupportedTypes" ".mpg" ""
WriteRegStr HKCR ".mpeg\OpenWithList\Vela.exe" "" ""
WriteRegStr HKCR "Applications\Vela.exe\SupportedTypes" ".mpeg" ""
WriteRegStr HKCR ".ts\OpenWithList\Vela.exe" "" ""
WriteRegStr HKCR "Applications\Vela.exe\SupportedTypes" ".ts" ""
WriteRegStr HKCR ".3gp\OpenWithList\Vela.exe" "" ""
WriteRegStr HKCR "Applications\Vela.exe\SupportedTypes" ".3gp" ""

; Still images: same OpenWithList + SupportedTypes registration as videos.
; Vela decodes them through FFmpeg's image2 demuxer and shows Vela as a
; secondary "Open with" option without displacing the default handler
; (Windows Photos / Paint / etc.).
WriteRegStr HKCR ".png\OpenWithList\Vela.exe" "" ""
WriteRegStr HKCR "Applications\Vela.exe\SupportedTypes" ".png" ""
WriteRegStr HKCR ".jpg\OpenWithList\Vela.exe" "" ""
WriteRegStr HKCR "Applications\Vela.exe\SupportedTypes" ".jpg" ""
WriteRegStr HKCR ".jpeg\OpenWithList\Vela.exe" "" ""
WriteRegStr HKCR "Applications\Vela.exe\SupportedTypes" ".jpeg" ""
WriteRegStr HKCR ".gif\OpenWithList\Vela.exe" "" ""
WriteRegStr HKCR "Applications\Vela.exe\SupportedTypes" ".gif" ""
WriteRegStr HKCR ".bmp\OpenWithList\Vela.exe" "" ""
WriteRegStr HKCR "Applications\Vela.exe\SupportedTypes" ".bmp" ""
WriteRegStr HKCR ".webp\OpenWithList\Vela.exe" "" ""
WriteRegStr HKCR "Applications\Vela.exe\SupportedTypes" ".webp" ""
WriteRegStr HKCR ".tif\OpenWithList\Vela.exe" "" ""
WriteRegStr HKCR "Applications\Vela.exe\SupportedTypes" ".tif" ""
WriteRegStr HKCR ".tiff\OpenWithList\Vela.exe" "" ""
WriteRegStr HKCR "Applications\Vela.exe\SupportedTypes" ".tiff" ""

; Poke Explorer so the change appears without a reboot.
System::Call 'shell32::SHChangeNotify(i 0x08000000, i 0, i 0, i 0)'
