<p align="center"><img src="https://raw.githubusercontent.com/Project-Chronicles-Emulator/game-server/main/docs/assets/project-chronicles-hero.png" alt="Project Chronicles fantasy harbor" width="100%"></p>

<h1 align="center">Project Chronicles · Login Server</h1>

<p align="center"><a href="https://github.com/Project-Chronicles-Emulator/game-server/blob/main/README.md">Project home</a> · <a href="https://github.com/Project-Chronicles-Emulator/game-server/blob/main/docs/guides/getting-started.md">Complete setup</a> · <a href="https://github.com/Project-Chronicles-Emulator/game-server/blob/main/docs/progress.md">Progress</a> · <a href="https://github.com/Project-Chronicles-Emulator/game-server/blob/main/CONTRIBUTING.md">Contribute</a></p>

Account authentication, sessions, launcher grants and single-use world admission. World simulation belongs to the separate game server.

## Build this component

Requires Windows x64, Visual Studio 2026 C++ tools, CMake 3.25+, Ninja, Python and
vcpkg. Install Python's build dependencies when building login:
`python -m pip install protobuf==6.33.6`. Desktop applications need WebView2 Runtime.
Each checkout includes its source dependencies; no sibling checkout is needed to compile.

```powershell
. .\tools\dev_shell.ps1
cmake --preset windows -DCMAKE_TOOLCHAIN_FILE=C:/dev/vcpkg/scripts/buildsystems/vcpkg.cmake
cmake --build --preset release --target chronicles-login-server
```

Replace the vcpkg path with your local installation. Keep the built executable,
DLLs and web assets together.

## Run

First complete the [shared environment setup](https://github.com/Project-Chronicles-Emulator/game-server/blob/main/docs/guides/getting-started.md).
It covers database installation, service keys, world TLS and startup order.

```powershell
.\out\build\windows\bin\Release\chronicles-login-server.exe --config config/services/login.example.json
```

Login public HTTP defaults to **8080**, private loopback authority to **8090**.
Set `CHRONICLES_DATABASE_URL` and `CHRONICLES_AUTH_SERVICE_KEY_FILE` privately.
Start login before game; both use the same database and service key.
See [local service details](SETUP.md) and [validation](VALIDATION.md).

## Work on the project

Read the [architecture](https://github.com/Project-Chronicles-Emulator/game-server/blob/main/docs/guides/architecture.md),
[feature/bug workflow](https://github.com/Project-Chronicles-Emulator/game-server/blob/main/docs/guides/feature-workflow.md) and
[testing guide](https://github.com/Project-Chronicles-Emulator/game-server/blob/main/docs/guides/testing.md). Report the exact component, commit,
reproduction and redacted evidence. Build/synthetic checks and original-client
observations are distinct; the service split has not had a fresh original-client
gameplay acceptance trial. Keep local credentials and generated/client files out of commits.

## Preview downloads and reproducible releases

The **Actions → Release preview** workflow builds a Windows x64 package from a
clean checkout and creates a draft prerelease with its SHA-256 checksum. Choose a
new `vMAJOR.MINOR.PATCH-preview.NUMBER` tag when dispatching it. A failed build does
not create a release. Published previews appear on this repository's Releases page.

Packages contain the application and selected support files only. They include no
client installation, extracted game data, archive keys or credentials. Keep the whole
package together; follow the setup instructions above before starting it. Windows
C++ applications require the Microsoft Visual C++ x64 runtime; desktop interfaces
also require Microsoft Edge WebView2 Runtime. The extractor package requires .NET 10.
These experimental previews do not establish compatibility with another client build.
