@echo off
cd /d "%~dp0"

set GCC="C:\Program Files (x86)\Arm GNU Toolchain arm-none-eabi\14.2 rel1\bin\arm-none-eabi-gcc.exe"
set OBJCOPY="C:\Program Files (x86)\Arm GNU Toolchain arm-none-eabi\14.2 rel1\bin\arm-none-eabi-objcopy.exe"
set SIZE="C:\Program Files (x86)\Arm GNU Toolchain arm-none-eabi\14.2 rel1\bin\arm-none-eabi-size.exe"

set VAPORWARE=%~dp0..\..\src

set CPU=-mcpu=cortex-m0 -mthumb
set INC=-I%VAPORWARE%\include
set CFLAGS=%CPU% %INC% -Os -ffunction-sections -fdata-sections -Wall -std=c11

if not exist build mkdir build

echo [1/2] startup.s
%GCC% %CPU% -x assembler-with-cpp -c %VAPORWARE%\src\startup.s -o build\startup.o || goto :error

echo [2/2] dump_ext_flash.c
%GCC% %CFLAGS% -c src\dump_ext_flash.c -o build\dump_ext_flash.o || goto :error

echo Linking...
%GCC% %CPU% -T%VAPORWARE%\n32g031.ld -Wl,--gc-sections -nostdlib -lnosys ^
  build\startup.o build\dump_ext_flash.o ^
  -o build\dump_ext_flash.elf || goto :error

%OBJCOPY% -O binary build\dump_ext_flash.elf build\dump_ext_flash.bin || goto :error
%SIZE% build\dump_ext_flash.elf

echo.
echo Build SUCCESS: build\dump_ext_flash.bin
goto :eof

:error
echo BUILD FAILED
exit /b 1
