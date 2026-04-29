# Build And Run VENPOD

This tutorial gets the tech demo running from a clean checkout.

## 1. Install prerequisites

Use a Windows machine with DirectX 12 support.

Install:

- Visual Studio 2022 with the C++ desktop workload
- CMake 3.20 or newer
- Git
- PowerShell

The setup script can install Ninja and vcpkg dependencies if they are missing. If you prefer to manage tools yourself, use the manual CMake path below.

## 2. Configure manually

Open a Visual Studio Developer PowerShell or Developer Command Prompt, then run from the repository root:

```powershell
cd VENPOD
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

## 3. Optional setup script

For a one-command local setup:

```powershell
.\setup.ps1
```

The script:

- imports the Visual Studio build environment
- installs or locates Ninja
- installs vcpkg packages
- checks `vendor/imgui`
- configures CMake
- builds `VENPOD.exe`

## 4. Run the demo

```powershell
.\run.ps1
```

Choose `Sandbox Mode` in the launcher. This is the infinite terrain explorer.

## 5. Rebuild after edits

```powershell
.\build.ps1
```

Use `.\clean.ps1` if you want to remove generated build files and configure again.

## Expected Result

The sandbox opens in a first-person view above generated voxel terrain. Nearby terrain streams in first, then the surrounding render window fills in. Mouse look and WASD movement should be smooth once the initial visible chunk window is populated.
