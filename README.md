# Onslaught <img src="assets/onslaught-readme.png" width="40" alt="Onslaught">

**Stretch your screen. Expand your view.**

Stretch games and applications across your screen while keeping your desktop at its normal resolution.

## Download

[**Download Onslaught**](https://github.com/stereolove33/Onslaught/releases/latest/download/Onslaught-Windows-x64.zip), extract the ZIP and open **Onslaught.exe**. Keep **OnslaughtEngine.exe** in the same folder.

## How it works

**Before using Onslaught, your desired stretched resolution must already be created in Windows / your graphics driver and selected inside the game or application.** Onslaught does not create custom resolutions or configure the application's internal rendering resolution.

For example: keep your desktop at **1920×1080**, set your game to **1680×1050 in windowed mode**, and use Onslaught to stretch that image to **1920×1080**, filling the screen.

1. Open your game or application in windowed mode at your desired resolution.
2. Select the application and output monitor in Onslaught.
3. Choose **Apps** for a mapped cursor or **FPS** for relative mouse input, then click **Start stretching**.

**Settings** lets you change the language, remember your last application, customize the emergency shortcut and check for updates. Alt+Tab releases input so you can use other applications.

Only one instance of Onslaught can run at a time. If it is already open, check the system tray to restore it.

To stop stretching, use your configured shortcut or **Ctrl+Alt+F12**. To exit Onslaught completely, select **Quit** from its system tray menu.

## Build

The source code is public under the [MIT license](LICENSE). To build Onslaught yourself, use **Windows x64**, **Visual Studio 2022 / Build Tools** with **Desktop development with C++**, **Windows SDK 10.0.19041 or newer**, and **CMake 3.24 or newer**.

Open a Developer PowerShell for Visual Studio and run:

```powershell
git clone https://github.com/stereolove33/Onslaught.git
cd Onslaught
cmake -S . -B build -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

The executables are in `build/Release`. Keep **Onslaught.exe** and **OnslaughtEngine.exe** together. The icon is embedded in the executable; no separate runtime image is required.

## Security info

- Uses Windows APIs for capture, presentation and cursor handling.
- Does not inject code or modify game memory or game files.
- Checks GitHub Releases for updates; downloads open the release page without automatically installing executables.
- Settings and logs stay inside the Onslaught folder. Update checks do not upload logs.
- Experimental software: compatibility, performance and permitted usage depend on the game or application.

[Source code](https://github.com/stereolove33/Onslaught) · [Report an issue](https://github.com/stereolove33/Onslaught/issues)

