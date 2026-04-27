@echo off
:: Flappy Bird build for N32G031 + GC9107 128x160 LCD
:: Uses vaporware shared library for display, system, battery, and vape drivers.
:: Excludes main.c / tamagotchi.c - uses flappy.c instead

set GCC="C:\Program Files (x86)\Arm GNU Toolchain arm-none-eabi\14.2 rel1\bin\arm-none-eabi-gcc.exe"
set OBJCOPY="C:\Program Files (x86)\Arm GNU Toolchain arm-none-eabi\14.2 rel1\bin\arm-none-eabi-objcopy.exe"
set SIZE="C:\Program Files (x86)\Arm GNU Toolchain arm-none-eabi\14.2 rel1\bin\arm-none-eabi-size.exe"

set VAPORWARE=%~dp0..\..\src

set CPU=-mcpu=cortex-m0 -mthumb
set INC=-I%VAPORWARE%\include
set CFLAGS=%CPU% %INC% -Os -ffunction-sections -fdata-sections -Wall -std=c11 -DFLAPPY_BIRD

if not exist build mkdir build

echo [1/7] startup.s
%GCC% %CPU% -x assembler-with-cpp -c src\startup.s -o build\startup_flappy.o || goto :error

echo [2/7] system.c  (vaporware: clock + TIM3 + TIM1)
%GCC% %CFLAGS% -c %VAPORWARE%\src\system.c -o build\system_flappy.o || goto :error

echo [3/7] display.c  (vaporware: GC9107 LCD driver)
%GCC% %CFLAGS% -c %VAPORWARE%\src\display.c -o build\display_flappy.o || goto :error

echo [4/7] battery.c  (vaporware: ADC battery reader)
%GCC% %CFLAGS% -c %VAPORWARE%\src\battery.c -o build\battery_flappy.o || goto :error

echo [5/7] vape.c  (vaporware: coil fire control)
%GCC% %CFLAGS% -c %VAPORWARE%\src\vape.c -o build\vape_flappy.o || goto :error

echo [6/7] flappy.c
%GCC% %CFLAGS% -c src\flappy.c -o build\flappy.o || goto :error

echo [7/7] Linking...
%GCC% %CPU% -Tn32g031.ld -Wl,--gc-sections -Wl,-Map=build\flappy.map -nostdlib -lnosys ^
  build\startup_flappy.o build\system_flappy.o build\display_flappy.o ^
  build\battery_flappy.o build\vape_flappy.o build\flappy.o ^
  -o build\flappy.elf || goto :error

%OBJCOPY% -O binary build\flappy.elf build\flappy.bin || goto :error
%OBJCOPY% -O ihex   build\flappy.elf build\flappy.hex || goto :error
%SIZE% build\flappy.elf

echo.
echo Build SUCCESS: build\flappy.bin
echo.
echo Next: python gen_direct_flash.py   then   flash_vape.bat
goto :eof

:error
echo BUILD FAILED
exit /b 1