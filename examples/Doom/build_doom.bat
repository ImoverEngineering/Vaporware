@echo off
cd /d "%~dp0"
:: build_doom.bat - Vaporware Doom-ish mini game
set APP_NAME=doom
set GCC="C:\Program Files (x86)\Arm GNU Toolchain arm-none-eabi\14.2 rel1\bin\arm-none-eabi-gcc.exe"
set OBJCOPY="C:\Program Files (x86)\Arm GNU Toolchain arm-none-eabi\14.2 rel1\bin\arm-none-eabi-objcopy.exe"
set SIZE="C:\Program Files (x86)\Arm GNU Toolchain arm-none-eabi\14.2 rel1\bin\arm-none-eabi-size.exe"
set VAPORWARE=%~dp0..\..\src
set CPU=-mcpu=cortex-m0 -mthumb
set INC=-I%VAPORWARE%\include -Iinclude
set CFLAGS=%CPU% %INC% -Os -ffunction-sections -fdata-sections -Wall -std=c11
if not exist build mkdir build

echo [1/12] startup.s (vaporware)
%GCC% %CPU% -x assembler-with-cpp -c %VAPORWARE%\src\startup.s -o build\startup.o || goto :error
echo [2/12] system.c (vaporware)
%GCC% %CFLAGS% -c %VAPORWARE%\src\system.c -o build\system.o || goto :error
echo [3/12] display.c (vaporware)
%GCC% %CFLAGS% -c %VAPORWARE%\src\display.c -o build\display.o || goto :error
echo [4/12] vape.c (vaporware)
%GCC% %CFLAGS% -c %VAPORWARE%\src\vape.c -o build\vape.o || goto :error
echo [5/12] button.c (vaporware)
%GCC% %CFLAGS% -c %VAPORWARE%\src\button.c -o build\button.o || goto :error
echo [6/12] battery.c (vaporware)
%GCC% %CFLAGS% -c %VAPORWARE%\src\battery.c -o build\battery.o || goto :error
echo [7/12] nv.c (vaporware)
%GCC% %CFLAGS% -c %VAPORWARE%\src\nv.c -o build\nv.o || goto :error
echo [8/12] app.c (vaporware)
%GCC% %CFLAGS% -c %VAPORWARE%\src\app.c -o build\app.o || goto :error
echo [9/12] doom_title_letterbox.c (title art)
%GCC% %CFLAGS% -c src\doom_title_letterbox.c -o build\doom_title_letterbox.o || goto :error
echo [10/12] doom_enemy_sprites.c (enemy sheet)
%GCC% %CFLAGS% -c src\doom_enemy_sprites.c -o build\doom_enemy_sprites.o || goto :error
echo [11/12] doom_deathscreen.c (death screen)
%GCC% %CFLAGS% -c src\doom_deathscreen.c -o build\doom_deathscreen.o || goto :error
echo [12/12] main.c (app)
%GCC% %CFLAGS% -c src\main.c -o build\main.o || goto :error

echo Linking...
%GCC% %CPU% -T%VAPORWARE%\n32g031.ld -Wl,--gc-sections -Wl,-Map=build\%APP_NAME%.map -nostdlib -lnosys ^
  build\startup.o build\system.o build\display.o build\vape.o ^
  build\button.o build\battery.o build\nv.o build\app.o ^
  build\doom_title_letterbox.o build\doom_enemy_sprites.o build\doom_deathscreen.o ^
  build\main.o ^
  -o build\%APP_NAME%.elf || goto :error
%OBJCOPY% -O binary build\%APP_NAME%.elf build\%APP_NAME%.bin || goto :error
%OBJCOPY% -O ihex build\%APP_NAME%.elf build\%APP_NAME%.hex || goto :error
%SIZE% build\%APP_NAME%.elf
echo.
echo Build SUCCESS: build\%APP_NAME%.bin
goto :eof
:error
echo BUILD FAILED
exit /b 1
