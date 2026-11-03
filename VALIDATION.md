# Service split validation

All four repository checkouts compiled independently with the pinned dependencies.
Only the dependency installation/cache was shared; no sibling source checkout was
required by their CMake targets.

- Login: full build and CTest inventory of 261 tests. Across the standard run and
  correctly provisioned isolated PostgreSQL fixtures, 250 tests passed. Eleven
  opt-in stress/fault/restart scenarios were not executed.
- Game: 69 authentication tests and 10 native lobby/front tests passed.
- Launcher and admin: standalone Release builds passed. Admin's initial local
  address is game HTTP port 8082; existing saved settings are preserved.
- Independent login/game executable processes passed registration, launcher login
  and grants, native HTTP and auth WebSocket realm entry, character creation and
  selection, both world TLS admission credential formats and ticket replay rejection.
- Private request/reply encryption, request replay rejection, public/private listener
  isolation, separate database leases, duplicate-owner rejection, login outage,
  login restart and game heartbeat expiry were verified on an isolated PostgreSQL
  cluster using the ordinary application role.

Three tests initially failed because a shared fixture lacked their required setup.
They passed after using their dedicated fixtures: a fresh Discord account database,
the named loot-claim database, and an explicitly installed inactive skill-item test
schema. That inactive schema is not part of normal installation; the public main
installer retains schema 44. No production database or running production service
was modified by these checks.

These are synthetic unit, database and transport results. Original-client gameplay
acceptance after this service split has not been performed. Existing historical
client acceptance records describe earlier builds and must not be treated as a
new client trial.
