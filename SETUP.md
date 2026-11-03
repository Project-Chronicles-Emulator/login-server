# Local service setup

Build with Visual Studio 2026 C++ tools, CMake 3.25+, Ninja, Python and the pinned
vcpkg manifest. This checkout contains its own source dependencies.

```powershell
. .\tools\dev_shell.ps1
cmake --preset windows -DCMAKE_TOOLCHAIN_FILE=C:/path/to/vcpkg/scripts/buildsystems/vcpkg.cmake
cmake --build --preset release --target chronicles-login-server chronicles-roster-tests
ctest --preset release -R "AuthAuthority|SplitAuthTest|NativeAuth|AuthService|LauncherRoster"
```

Install PostgreSQL 16 and run `pwsh -File tools/install_database.ps1` once. Both
services use the same current schema and database. Set `CHRONICLES_DATABASE_URL`
privately in both service environments using the dedicated application role.

Create a random 32-byte key as 64 lowercase hex digits in a private file outside
source control. Set `CHRONICLES_AUTH_SERVICE_KEY_FILE` to that same file for both
services. Restrict file access to their operating-system account. Never commit it.

```powershell
.\out\build\windows\bin\Release\chronicles-login-server.exe --config config/services/login.example.json
```

Start login before game. Public login defaults to `http://127.0.0.1:8080` and the
game front address to `http://127.0.0.1:8082`. Private authority binds only loopback
on `8090`. Do not expose or forward that port. Remote public bindings require TLS
and a matching remote HTTPS origin. The initial split requires one host.

Private messages encrypt and authenticate payloads and bind the operation,
database identity and request ID. Stale and replayed requests are rejected, and
replies are authenticated before their status or payload is trusted. The master
key is never transmitted.

`/health` reports service health and heartbeat-derived realm availability. Realm
entry closes after five seconds without a fresh game heartbeat. Sessions are
process-local: restarting login requires authentication again. Durable reconnect
credentials preserve their existing password-reset and ban behavior. Login
outages fail new authentication and admission closed without stopping simulation.

Game owns character and world state. The remote admission commit is the
cross-service authorization point; revocation or reselection before it rejects
entry. Final socket admission uses the locally prepared result without network
I/O on its executor. Existing world connections retain game-side disconnect and
revocation handling.

Validation uses separate instances, loopback HTTP, isolated PostgreSQL, two
operating-system processes and both TLS admission credential formats. These are
synthetic service/transport checks, not original-client gameplay acceptance.
Independent databases, multi-host authority transport and replicated login
sessions are not implemented.
