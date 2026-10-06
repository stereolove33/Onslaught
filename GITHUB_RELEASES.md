# Publishing updates

Repository: https://github.com/stereolove33/Onslaught

Onslaught uses stereolove33/Onslaught by default. The repository can be changed in Settings. Update checks request the latest published stable release from GitHub without authentication. Ordinary tags, draft releases and prereleases are not included.

## Build and test

Install Visual Studio 2022 or Build Tools with Desktop development with C++, CMake and Windows SDK 10.0.19041 or newer. Run build.cmd from an x64 Developer Command Prompt. The executables are written to build/Release; keep both in the same folder.

The CTest suite checks cursor mapping, cursor rasterization, interface navigation and language selection, repository validation, remembered application selection and release version parsing.

## Publish a release

1. Update Version in src/release.h, the resource version in assets/app.rc and the displayed interface version in src/ui.cpp.
2. Commit and push the source changes.
3. Create a tag matching the application version, such as v1.0.0.

.github/workflows/release.yml builds Windows x64, runs the tests and uploads Onslaught-Windows-x64.zip and its SHA-256 checksum to the tagged release. It can also be run manually from Actions. If the release already exists, the workflow attaches the verified build to it.

.gitignore excludes settings.ini, logs, local builds and executables. Do not commit personal settings or diagnostic logs. Application icons are embedded during compilation.

Update checks use https://api.github.com/repos/OWNER/REPO/releases/latest. See the official API documentation: https://docs.github.com/en/rest/releases/releases#get-the-latest-release. The interface reports network errors, missing releases, unsupported version tags and rate limits. It opens the release page for download and does not replace running executables.

