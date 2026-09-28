# PD2 VR Normal Chat DLL

Native SuperBLT plugin for PAYDAY 2 VR chat input.

This repository contains the C++ DLL used by the main PAYDAY 2 VR chat mod.

## Requirements

- Windows
- Visual Studio 2022 with `Desktop development with C++`
- CMake 3.24 or newer
- Git
- Internet connection during the first build

The project is built for `x64`.

## Dependencies

Dependencies are downloaded automatically by CMake through `FetchContent`.

The project uses:

- PAYDAY 2 SuperBLT DLL Template
- Valve OpenVR SDK

No manual dependency setup is required.

## Project structure

```text
src/
├─ main.cpp
├─ legal.cpp
└─ exports.def

build/
└─ build.ps1

CMakeLists.txt
