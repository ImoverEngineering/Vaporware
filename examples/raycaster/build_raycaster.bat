@echo off
:: build_raycaster.bat - Raycaster for N32G031 + GC9107 128x160 LCD

set GCC="C:\Program Files (x86)\Arm GNU Toolchain arm-none-eabi\14.2 rel1\bin\arm-none-eabi-gcc.exe"
set OBJCOPY="C:\Program Files (x86)\Arm GNU Toolchain arm-none-eabi\14.2 rel1\bin\arm-none-eabi-objcopy.exe"
set SIZE="C:\Program Files (x86)\Arm GNU Toolchain arm-none-eabi\14.2 rel1\bin\arm-none-eabi-size.exe"

set VAPORWARE=%~dp0..\..\src

set CPU=-mcpu=cortex-m0 -mthumb
set INC=-I%VAPORWARE%\include
set CFLAGS=%CPU% %INC% -Os -ffunction-sections -fdata-sections -Wall -std=c11

if not exist build mkdir build

echo [1/5] startup.s
%GCC% %CPU% -x assembler-with-cpp -c src\startup.s -o build\startup.o || goto :error

echo [2/5] system.c
%GCC% %CFLAGS% -c %VAPORWARE%\src\system.c -o build\system.o || goto :error

echo [3/5] display.c
%GCC% %CFLAGS% -c %VAPORWARE%\src\display.c -o build\display.o || goto :error

echo [4/5] battery.c
%GCC% %CFLAGS% -c %VAPORWARE%\src\battery.c -o build\battery.o || goto :error

echo [5/5] raycaster.c + link
%GCC% %CFLAGS% -c src\raycaster.c -o build\raycaster.o || goto :error

%GCC% %CPU% -Tn32g031.ld -Wl,--gc-sections -Wl,-Map=build\raycaster.map -nostdlib -lnosys ^
  build\startup.o build\system.o build\display.o build\battery.o build\raycaster.o ^
  -o build\raycaster.elf || goto :error

%OBJCOPY% -O binary build\raycaster.elf build\raycaster.bin || goto :error
%OBJCOPY% -O ihex   build\raycaster.elf build\raycaster.hex || goto :error
%SIZE% build\raycaster.elf

echo.
echo Build SUCCESS: build\raycaster.bin
echo Next: python gen_direct_flash.py   then   flash_vape.bat
goto :eof

:error
echo BUILD FAILED
exit /b 1
