# L2D3D12Portable

Portable y reversible **Direct3D 9 -> Direct3D 12** initializer for 32-bit
Lineage II C4, Interlude and similar private-server clients.

Inicializador portable y reversible **Direct3D 9 -> Direct3D 12** para clientes
Lineage II C4, Interlude y derivados de 32 bits.

> This is a compatibility experiment, not a native DirectX 12 renderer and not
> an Unreal Engine 2 multithreading modification.

## Download / Descarga

Download the ready-to-use ZIP from the
[latest release](https://github.com/HeKoXCode/L2D3D12Portable/releases/latest).

Descarga el ZIP listo para usar desde la
[ultima version](https://github.com/HeKoXCode/L2D3D12Portable/releases/latest).

1. Extract the ZIP.
2. Double-click `Start-L2D3D12.cmd` and accept the UAC prompt.
3. Select the complete Lineage II client or its `system` directory.
4. Select `L2.exe`, the server updater or its custom launcher from the file
   picker.
5. Use `Remove-L2D3D12.cmd` to remove the installed proxy safely.

No PowerShell command or fixed installation path is required. The CMD finds its
own portable directory and requests administrator rights automatically so that
an elevated `L2.exe` inherits the D3D9On12 environment.

The launcher must be used again for later DX12 sessions because backend
selection is intentionally passed only to the process it starts.

## What it actually does

- Finds a unique client `system` directory containing `L2.exe`.
- Verifies that the client is PE32/x86.
- Parses top-level EXE/DLL import tables instead of guessing from a folder name.
- Supports `Direct3DCreate9`, `Direct3DCreate9Ex` and the common `D3DPERF_*`
  imports exported by Windows D3D9.
- Runs a hidden GPU/OS preflight and proves D3D9On12 through
  `IDirect3DDevice9On12` before changing the client.
- Places a local `d3d9.dll` proxy in `system` and asks the Windows D3D9On12
  component to create the translated backend.
- Logs whether the requested backend became D3D9On12 or fell back to native
  D3D9.
- Lets the user select a private-server updater or launcher without editing a
  command.
- Records a manifest and SHA-256 so removal only touches the exact file this
  tool installed.

Microsoft ships D3D9On12 as a Windows component. This project calls the OS
implementation; it does not ship or replace Microsoft's D3D9On12 binaries.
See [microsoft/D3D9On12](https://github.com/microsoft/D3D9On12).

## Safety and compatibility limits

- An existing unowned `system\d3d9.dll` is never overwritten. It may belong to
  ReShade, dxwrapper, another mod or a server protection system.
- Unsupported D3D9 imports fail closed before installation.
- Packed clients or clients that load D3D9 dynamically may not be detectable.
- An anti-cheat may reject local wrapper DLLs or client hash changes. This
  project does not bypass anti-cheat protections.
- D3D9On12 does not guarantee higher FPS. Translation can add overhead. In one
  Interlude login-screen A/B test, base FPS was approximately 9.2% lower than
  native D3D9; other hardware and scenes may behave differently.
- AFMF availability depends on the AMD driver, GPU, profile, display mode and
  current driver rules. A confirmed D3D12 device is not proof that AFMF is on.

Inspect without modifying the client:

```powershell
.\Start-L2D3D12.ps1 -ClientPath 'D:\Games\MyL2' -InspectOnly
```

Use a private-server launcher stored inside the client directory:

```powershell
.\Start-L2D3D12.ps1 `
  -ClientPath 'D:\Games\MyL2' `
  -LaunchExecutable 'Launcher.exe'
```

More usage details are included in [`portable/README.md`](portable/README.md).

## Building

Requirements:

- Windows 10/11
- Visual Studio 2022 Build Tools with Desktop development with C++
- Windows 10/11 SDK
- CMake 3.24 or newer

```powershell
cmake --preset vs2022-x86
cmake --build --preset vs2022-x86-relwithdebinfo
ctest --test-dir build\vs2022-x86 -C RelWithDebInfo --output-on-failure
```

The project deliberately rejects non-x86 builds because classic C4/Interlude
clients are 32-bit processes.

## Repository contents

- `src/`: D3D9 proxy, D3D9On12 selection and backend logging.
- `tests/`: native D3D9/D3D9On12 smoke test.
- `portable/`: ready-to-run launcher and compiled x86 binaries.
- `.github/workflows/build.yml`: reproducible x86 build check.

No Lineage II executable, game asset, server file or NCSoft/Epic source code is
included.

## License

MIT. See [`LICENSE`](LICENSE).
