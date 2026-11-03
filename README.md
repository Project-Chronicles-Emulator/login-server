# Project Chronicles login server

This repository is reserved for the autonomous authentication service currently being
extracted from the combined server. It does not yet contain a runnable login server.

## Service boundary

The login service owns account authentication, password and Discord login, session
issuance and renewal, realm discovery, and short-lived, single-use world admission.
The game service owns character and world state, simulation, combat, quests, inventory,
NPCs, and the live world connection registry.

The game service must verify account, character and world ownership before consuming
an admission. Expired, replayed, revoked, superseded or malformed admissions fail closed.
A password reset, ban or operator sign-out must invalidate pending admissions across
processes. The authenticated game connection must be visible to operator revocation
before its final admission claim succeeds.

The services need a versioned private admission contract and independent startup,
configuration, health, shutdown and restart behavior. Passwords and database credentials
must never be passed to the game client. Existing original-client framing, keys, packet
ordering, launch grants and reconnect behavior remain acceptance requirements.

## Extraction status

Repository created. Runtime extraction and cross-process verification remain pending.
The existing combined server remains the working behavioral baseline; copying its
in-memory sessions into two processes is not a supported deployment.
