# Windows build guide

This guide explains how to compile `ground_control_windows.c` on Windows.

## 1. Install build tools

Recommended option: install MSYS2, then open **MSYS2 MinGW 64-bit** terminal and run:

```sh
pacman -S --needed mingw-w64-x86_64-gcc mingw-w64-x86_64-libusb
```

Make sure the MinGW binary directory is in your `PATH`, for example:

```powershell
$env:Path += ";C:\msys64\mingw64\bin"
```

## 2. Compile

From the repository root, enter this directory:

```powershell
cd sniffer_storage
```

Compile with GCC:

```powershell
gcc -Wall -Wextra -O2 -o ground_control_windows.exe ground_control_windows.c -lusb-1.0
```

If GCC cannot find `libusb.h` or `libusb-1.0`, give the include and library paths explicitly:

```powershell
gcc -Wall -Wextra -O2 -IC:\msys64\mingw64\include -LC:\msys64\mingw64\lib -o ground_control_windows.exe ground_control_windows.c -lusb-1.0
```

## 3. Run

Keep `libusb-1.0.dll` available at runtime. The simplest way is to run from a terminal where `C:\msys64\mingw64\bin` is already in `PATH`.

```powershell
.\ground_control_windows.exe
```

If the program cannot open or claim the USB device, install the WinUSB driver for the device interface with Zadig, then run the program again.
