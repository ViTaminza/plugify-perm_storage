# perm_storage (Plugify)

Persistence backend for the official permissions plugin: stores users (immunity, name, timestamps), groups, group permissions and options, user permissions, user groups and user cookies in MySQL or SQLite. Loads the groups into the core at startup, answers the core's user-load requests asynchronously and mirrors every *Storage event back into the database. No exported methods; install it next to a connector plugin (e.g. perm_connector) that calls LoadUser.

Original plugin (not a port).

## Requirements
- [Plugify](https://github.com/untrustedmodders/plugify) with [s2sdk](https://github.com/untrustedmodders/plugify-plugin-s2sdk)
- Plugify dependencies: permissions

## Installation
Download `perm_storage-1.0.1.zip` from Releases and extract it into `game/csgo/`.

## Building
Requires Docker. `./build.sh` builds in the Steam Runtime 3 (sniper) SDK image; the package
is written to `build/package/perm_storage`.

## License
GPL-3.0, see LICENSE.

