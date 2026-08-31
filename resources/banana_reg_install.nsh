; -----------------------------------------------------------------------------
; File association + "Open With" registration for Banana Player.
;
; Included from CPACK_NSIS_EXTRA_INSTALL_COMMANDS via `!include` so the raw
; NSIS text bypasses the CPackConfig.cmake string round-trip (which strips
; the literal quotes around $INSTDIR\bin\BananaPlayer.exe and breaks
; shell\open\command).
;
; Quoting reminder: inside NSIS "..." strings, `\` is literal; `$\"`
; is a literal double quote. Inside '...' strings, `\` is also literal,
; and inner double quotes are literal too - which is why the two lines
; that need embedded quotes (DefaultIcon and shell\open\command) use
; single-quote wrapping.
; -----------------------------------------------------------------------------

WriteRegStr HKCR "Applications\BananaPlayer.exe" "FriendlyAppName" "Banana Player"
WriteRegStr HKCR "Applications\BananaPlayer.exe" "ApplicationName" "Banana Player"
WriteRegStr HKCR "Applications\BananaPlayer.exe" "ApplicationCompany" "Banana Player"
WriteRegStr HKCR "Applications\BananaPlayer.exe" "ApplicationDescription" "Peel and play - lightweight, keyboard-first video playback"
WriteRegStr HKCR "Applications\BananaPlayer.exe\DefaultIcon" "" '"$INSTDIR\bin\BananaPlayer.exe",0'
WriteRegStr HKCR "Applications\BananaPlayer.exe\shell\open\command" "" '"$INSTDIR\bin\BananaPlayer.exe" "%1"'
WriteRegStr HKCR "Applications\BananaPlayer.exe\shell\open\FriendlyAppName" "" "Banana Player"
WriteRegStr HKLM "SOFTWARE\Microsoft\Windows\CurrentVersion\App Paths\BananaPlayer.exe" "" "$INSTDIR\bin\BananaPlayer.exe"
WriteRegStr HKLM "SOFTWARE\Microsoft\Windows\CurrentVersion\App Paths\BananaPlayer.exe" "Path" "$INSTDIR\bin"

WriteRegStr HKCR ".mp4\OpenWithList\BananaPlayer.exe" "" ""
WriteRegStr HKCR "Applications\BananaPlayer.exe\SupportedTypes" ".mp4" ""
WriteRegStr HKCR ".mkv\OpenWithList\BananaPlayer.exe" "" ""
WriteRegStr HKCR "Applications\BananaPlayer.exe\SupportedTypes" ".mkv" ""
WriteRegStr HKCR ".avi\OpenWithList\BananaPlayer.exe" "" ""
WriteRegStr HKCR "Applications\BananaPlayer.exe\SupportedTypes" ".avi" ""
WriteRegStr HKCR ".mov\OpenWithList\BananaPlayer.exe" "" ""
WriteRegStr HKCR "Applications\BananaPlayer.exe\SupportedTypes" ".mov" ""
WriteRegStr HKCR ".webm\OpenWithList\BananaPlayer.exe" "" ""
WriteRegStr HKCR "Applications\BananaPlayer.exe\SupportedTypes" ".webm" ""
WriteRegStr HKCR ".wmv\OpenWithList\BananaPlayer.exe" "" ""
WriteRegStr HKCR "Applications\BananaPlayer.exe\SupportedTypes" ".wmv" ""
WriteRegStr HKCR ".flv\OpenWithList\BananaPlayer.exe" "" ""
WriteRegStr HKCR "Applications\BananaPlayer.exe\SupportedTypes" ".flv" ""
WriteRegStr HKCR ".m4v\OpenWithList\BananaPlayer.exe" "" ""
WriteRegStr HKCR "Applications\BananaPlayer.exe\SupportedTypes" ".m4v" ""
WriteRegStr HKCR ".mpg\OpenWithList\BananaPlayer.exe" "" ""
WriteRegStr HKCR "Applications\BananaPlayer.exe\SupportedTypes" ".mpg" ""
WriteRegStr HKCR ".mpeg\OpenWithList\BananaPlayer.exe" "" ""
WriteRegStr HKCR "Applications\BananaPlayer.exe\SupportedTypes" ".mpeg" ""
WriteRegStr HKCR ".ts\OpenWithList\BananaPlayer.exe" "" ""
WriteRegStr HKCR "Applications\BananaPlayer.exe\SupportedTypes" ".ts" ""
WriteRegStr HKCR ".3gp\OpenWithList\BananaPlayer.exe" "" ""
WriteRegStr HKCR "Applications\BananaPlayer.exe\SupportedTypes" ".3gp" ""

; Still images: same OpenWithList + SupportedTypes registration as videos.
; BananaPlayer decodes them through FFmpeg's image2 demuxer and shows
; itself as a secondary "Open with" option without displacing the default
; handler (Windows Photos / Paint / etc.).
WriteRegStr HKCR ".png\OpenWithList\BananaPlayer.exe" "" ""
WriteRegStr HKCR "Applications\BananaPlayer.exe\SupportedTypes" ".png" ""
WriteRegStr HKCR ".jpg\OpenWithList\BananaPlayer.exe" "" ""
WriteRegStr HKCR "Applications\BananaPlayer.exe\SupportedTypes" ".jpg" ""
WriteRegStr HKCR ".jpeg\OpenWithList\BananaPlayer.exe" "" ""
WriteRegStr HKCR "Applications\BananaPlayer.exe\SupportedTypes" ".jpeg" ""
WriteRegStr HKCR ".gif\OpenWithList\BananaPlayer.exe" "" ""
WriteRegStr HKCR "Applications\BananaPlayer.exe\SupportedTypes" ".gif" ""
WriteRegStr HKCR ".bmp\OpenWithList\BananaPlayer.exe" "" ""
WriteRegStr HKCR "Applications\BananaPlayer.exe\SupportedTypes" ".bmp" ""
WriteRegStr HKCR ".webp\OpenWithList\BananaPlayer.exe" "" ""
WriteRegStr HKCR "Applications\BananaPlayer.exe\SupportedTypes" ".webp" ""
WriteRegStr HKCR ".tif\OpenWithList\BananaPlayer.exe" "" ""
WriteRegStr HKCR "Applications\BananaPlayer.exe\SupportedTypes" ".tif" ""
WriteRegStr HKCR ".tiff\OpenWithList\BananaPlayer.exe" "" ""
WriteRegStr HKCR "Applications\BananaPlayer.exe\SupportedTypes" ".tiff" ""

; Audio-only containers: same OpenWithList + SupportedTypes registration
; as videos. VideoPlayer::loadFile succeeds without a video stream and
; UIController::drawAudioOnlyPlaceholder fills the empty video area with
; the filename, so voice memos and music files play through the normal
; seek/volume/OSD stack.
WriteRegStr HKCR ".m4a\OpenWithList\BananaPlayer.exe" "" ""
WriteRegStr HKCR "Applications\BananaPlayer.exe\SupportedTypes" ".m4a" ""
WriteRegStr HKCR ".mp3\OpenWithList\BananaPlayer.exe" "" ""
WriteRegStr HKCR "Applications\BananaPlayer.exe\SupportedTypes" ".mp3" ""
WriteRegStr HKCR ".flac\OpenWithList\BananaPlayer.exe" "" ""
WriteRegStr HKCR "Applications\BananaPlayer.exe\SupportedTypes" ".flac" ""
WriteRegStr HKCR ".wav\OpenWithList\BananaPlayer.exe" "" ""
WriteRegStr HKCR "Applications\BananaPlayer.exe\SupportedTypes" ".wav" ""
WriteRegStr HKCR ".aac\OpenWithList\BananaPlayer.exe" "" ""
WriteRegStr HKCR "Applications\BananaPlayer.exe\SupportedTypes" ".aac" ""
WriteRegStr HKCR ".opus\OpenWithList\BananaPlayer.exe" "" ""
WriteRegStr HKCR "Applications\BananaPlayer.exe\SupportedTypes" ".opus" ""
WriteRegStr HKCR ".ogg\OpenWithList\BananaPlayer.exe" "" ""
WriteRegStr HKCR "Applications\BananaPlayer.exe\SupportedTypes" ".ogg" ""

; Poke Explorer so the change appears without a reboot.
System::Call 'shell32::SHChangeNotify(i 0x08000000, i 0, i 0, i 0)'
