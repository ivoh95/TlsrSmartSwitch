@set TLPATH=D:\MCU\TelinkIoTStudio
@set PATH=%TLPATH%\bin;%TLPATH%\opt\tc32\bin;%TLPATH%\mingw\bin;%TLPATH%\opt\tc32\tc32-elf\bin;%PATH%
@set SWVER=_v0102
@rem Single-target build for the battery-powered ionizer. Unlike WinMakeZ.cmd
@rem this does NOT wipe .\bin - the mains-powered board images live there too.
@rem ZB_DEVICE_ROLE=ed selects -lzb_ed and -DEND_DEVICE=1, which has to match
@rem the ZB_ED_ROLE that USE_BATTERY_PM turns on in app_cfg.h for this board.
set PROJECT_NAME=DIY_ION
make -s -j clean
make -s -j VERSION_BIN=%SWVER% PROJECT_NAME=%PROJECT_NAME% ZB_DEVICE_ROLE=ed POJECT_DEF="-DBOARD=BOARD_%PROJECT_NAME%"
@if not exist "bin\%PROJECT_NAME%%SWVER%.bin" goto :error
@echo Built bin\%PROJECT_NAME%%SWVER%.bin
@exit /b 0
:error
@echo "Error!"
@exit /b 1
