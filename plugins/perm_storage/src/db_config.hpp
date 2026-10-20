// Database connection settings of the perm_storage plugin (PLUGIN_STANDARD.md section 4).
//
// The connection is read from <GetConfigsDir()>/perm_storage/databases.cfg - in the Pisex spelling
// "addons/configs/databases.cfg", which pisexcompat maps to the plugin's config directory - from
// the section named after the plugin ("perm_storage"): the same file, section and key names the
// reference ports (plugins/vip, plugins/admin_system) read.
//
// During the transition period a server that has no databases.cfg yet keeps working: the database
// keys of configs/perm_storage/config.jsonc are used and one warning says where the connection has
// to move (PLUGIN_STANDARD.md section 6).
//
// The reader needs no engine (no s2sdk, no plugify calls besides the config/data directories), so
// tests/db_config_tests.cpp drives it without a server.
#pragma once

#include <string>
#include <string_view>

#include <plgdb/db.hpp>

namespace perm_storage {

// Where the returned connection settings were read from.
enum class DatabaseSource {
	DatabasesCfg, // the "perm_storage" section of databases.cfg (the standard)
	LegacyConfig, // the database keys of config.jsonc (transition period only)
};

struct DatabaseConfig {
	plgdb::Config db;
	DatabaseSource source = DatabaseSource::LegacyConfig;
};

// Decides the connection the plugin starts with:
//   1. the "perm_storage" section of <GetConfigsDir()>/perm_storage/databases.cfg when the file has
//      it;
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
