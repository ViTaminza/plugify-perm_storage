// Database connection settings of the perm_storage plugin (PLUGIN_STANDARD.md section 4).
//
// The connection is read from a databases.cfg file in the same order the pre-standard reader got
// from pisexcompat's path shim for the Metamod spelling "addons/configs/databases.cfg":
//   1. "addons/configs/databases.cfg" relative to the server working directory;
//   2. <GetConfigsDir()>/perm_storage/databases.cfg (the standard plugin location);
//   3. <GetConfigsDir()>/perm_storage/translations/databases.cfg;
//   4. <GetDataDir()>/perm_storage/databases.cfg (`dataDir` is the plugify data root).
// The first file that exists is authoritative: a malformed file or a missing "perm_storage"
// section makes the plugin fall back to the legacy config.jsonc (with one warning) instead of
// silently taking another databases.cfg. The shared <GetConfigsDir()>/databases.cfg is never read:
// the old reader ignored it too, and it may belong to another plugin.
//
// The section is named after the plugin ("perm_storage"): the same file, section and key names the
// reference ports (plugins/vip, plugins/admin_system) read.
//
// The file is parsed by the plugin's own read-only KV1 reader (src/kv1.hpp), so this module needs
// no shim library and no engine. Only GetConfigsDir() is called through the official plugify API;
// the data directory is passed in by the caller, so tests/db_config_tests.cpp drives the whole
// precedence chain without a server.
//
// During the transition period a server that has no databases.cfg at all keeps working: the
// database keys of configs/perm_storage/config.jsonc are used and one warning says where the
// connection has to move (PLUGIN_STANDARD.md section 6).
#pragma once

#include <string>
#include <string_view>

#include <plgdb/db.hpp>

namespace perm_storage {

// Where the returned connection settings were read from.
enum class DatabaseSource {
	DatabasesCfg, // the "perm_storage" section of one of the databases.cfg candidates
	LegacyConfig, // the database keys of config.jsonc (transition period only)
};

struct DatabaseConfig {
	plgdb::Config db;
	DatabaseSource source = DatabaseSource::LegacyConfig;
};

// Decides the connection the plugin starts with:
//   1. the "perm_storage" section of the first existing databases.cfg of the old search order
//      (raw "addons/configs/databases.cfg", <GetConfigsDir()>/perm_storage/databases.cfg,
//      .../translations/databases.cfg, <dataDir>/perm_storage/databases.cfg). That file is
//      authoritative: when it is malformed or lacks the section, this function does NOT try the
//      next candidate - it falls back to the legacy config below and reports why in `warning`;
//   2. otherwise the database keys of the plugin config - `legacyPath` names the file in messages,
//      `legacyText` is its content - and the connection has to be moved to databases.cfg.
// Relative SQLite paths are resolved against `dataDir`; ":memory:" and absolute paths are kept,
// exactly like the pre-standard loader, so nothing moves the file of a running server. `dataDir`
// may be empty when no relative SQLite path is used.
// Returns false with `error` set on a hard failure (an unusable legacy config; the plugin then
// fails to start like before). `warning`, when non-empty, is the single message to log about the
// connection still living in the plugin config. Neither message carries a plugin-name prefix.
bool LoadDatabaseConfig(std::string_view legacyPath, std::string_view legacyText, std::string_view dataDir,
						DatabaseConfig& out, std::string& warning, std::string& error);

} // namespace perm_storage
