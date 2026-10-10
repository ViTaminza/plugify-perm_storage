#include "db_config.hpp"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <system_error>
#include <utility>

#include <nlohmann/json.hpp>

#include <pisex/keyvalues.hpp>
#include <pisex/paths.hpp>

namespace perm_storage {
namespace {

// The plugin name is also the name of its databases.cfg section (PLUGIN_STANDARD.md section 4).
constexpr const char* kConnectionName = "perm_storage";
// The Metamod spelling the original read; pisexcompat resolves it against <GetConfigsDir()>/<plugin>.
constexpr const char* kDatabasesCfgPath = "addons/configs/databases.cfg";

// The defaults of the pre-standard loader, kept so a config.jsonc without the keys behaves exactly
// as it did (and an empty config still means a local SQLite file next to the plugin data).
constexpr const char* kDefaultSqlitePath = "perm_storage.sqlite3";
constexpr const char* kDefaultDatabase = "perm_storage";

// The "perm_storage" section of a databases.cfg tree: the same keys, defaults and fail-soft reading
// as plugins/vip/src/plugin.cpp:226, plugins/admin_system/src/plugin.cpp:256 and
// plugins/levels_ranks/src/plugin.cpp:279 (anything but "sqlite" means MySQL). Only a missing
// section is a reason to fall back - a section with a broken driver or host is used as given, like
// the reference ports do.
bool ReadDatabasesCfgSection(pisex::KeyValues& root, DatabaseConfig& out, std::string& reason) {
	pisex::KeyValues* section = root.FindKey(kConnectionName);
	if (section == nullptr) {
		reason = std::string("databases.cfg has no '") + kConnectionName + "' section";
		return false;
	}

	plgdb::Config db;
	db.host = std::string(section->GetStringValue("host", "127.0.0.1"));
	db.port = static_cast<uint16_t>(std::max(1, std::min(65535, section->GetInt("port", 3306))));
	db.user = std::string(section->GetStringValue("user", ""));
	db.password = std::string(section->GetStringValue("pass", ""));
	db.database = std::string(section->GetStringValue("database", kDefaultDatabase));
	if (section->GetStringValue("driver", "mysql") == "sqlite") {
		db.driver = plgdb::Driver::SQLite;
		db.sqlitePath = std::string(section->GetStringValue("sqlite_path", kDefaultSqlitePath));
	} else {
		db.driver = plgdb::Driver::MySQL;
	}

	out.db = std::move(db);
	out.source = DatabaseSource::DatabasesCfg;
	return true;
}

// The transition-period source: the database keys of config.jsonc, parsed exactly like the loader
// in plugin.cpp did before the standard (same defaults, same hard errors).
bool ReadLegacyPluginConfig(std::string_view path, std::string_view text, DatabaseConfig& out,
							std::string& error) {
	nlohmann::json json;
	try {
		json = nlohmann::json::parse(text, nullptr, /*allow_exceptions=*/true, /*ignore_comments=*/true);
	} catch (const nlohmann::json::exception& ex) {
		error = "config parse error in '" + std::string(path) + "': " + ex.what();
		return false;
	}
	if (!json.is_object()) {
		error = "config '" + std::string(path) + "' must be a JSON object";
		return false;
	}

	plgdb::Config db;
	const std::string driver = json.value("driver", std::string("sqlite"));
	if (driver == "sqlite") {
		db.driver = plgdb::Driver::SQLite;
		db.sqlitePath = json.value("sqlite_path", std::string(kDefaultSqlitePath));
	} else if (driver == "mysql") {
		db.driver = plgdb::Driver::MySQL;
		db.host = json.value("host", std::string("127.0.0.1"));
		const int port = json.value("port", 3306);
		if (port < 1 || port > 65535) {
			error = "MySQL 'port' out of range (1..65535)";
			return false;
		}
		db.port = static_cast<uint16_t>(port);
		db.user = json.value("user", std::string());
		db.password = json.value("password", std::string());
		db.database = json.value("database", std::string(kDefaultDatabase));
		db.charset = json.value("charset", std::string("utf8mb4"));
	} else {
		error = "unknown 'driver' '" + driver + "' (expected \"sqlite\" or \"mysql\")";
		return false;
	}

	out.db = std::move(db);
	out.source = DatabaseSource::LegacyConfig;
	return true;
}

// Shared tail of both sources: a relative SQLite file lives under the plugin data directory, an
// empty path is an error - the checks the pre-standard loader made in plugin.cpp.
bool ResolveSqlitePath(DatabaseConfig& config, std::string_view dataDir, std::string& error) {
	if (config.db.driver != plgdb::Driver::SQLite) {
		return true;
	}
	if (config.db.sqlitePath.empty()) {
		error = "'sqlite_path' must not be empty";
		return false;
	}
	if (config.db.sqlitePath == ":memory:" || std::filesystem::path(config.db.sqlitePath).is_absolute()) {
		return true;
	}
	if (dataDir.empty()) {
		error = "plugify data directory is not available";
		return false;
	}
	config.db.sqlitePath = pisex::JoinPath(dataDir, config.db.sqlitePath);
	std::error_code ec;
	const std::filesystem::path parent = std::filesystem::path(config.db.sqlitePath).parent_path();
	if (!parent.empty()) {
		std::filesystem::create_directories(parent, ec);
		if (ec) {
			error = "cannot create data directory '" + parent.string() + "': " + ec.message();
			return false;
		}
	}
	return true;
}

} // namespace

bool LoadDatabaseConfig(std::string_view legacyPath, std::string_view legacyText, std::string_view dataDir,
						DatabaseConfig& out, std::string& warning, std::string& error) {
	warning.clear();
	// pisexcompat maps the Metamod path to <GetConfigsDir()>/<plugin> once the plugin name is set,
	// so the file read below is <configs>/perm_storage/databases.cfg and never the shared root one.
	pisex::SetPluginName(kConnectionName);

	std::string reason;
	DatabaseConfig chosen;
	pisex::KeyValues databases("Databases");
	if (databases.LoadFromFile(kDatabasesCfgPath)) {
		if (ReadDatabasesCfgSection(databases, chosen, reason)) {
			if (!ResolveSqlitePath(chosen, dataDir, error)) {
				return false;
			}
			out = std::move(chosen);
			return true;
		}
	} else {
		reason = "cannot read '" + pisex::ConfigDir() + "/databases.cfg'";
	}

	// Transition period (PLUGIN_STANDARD.md section 6): the connection is still where it was, so
	// the plugin keeps working, and the one warning below tells the owner where it has to move.
	if (!ReadLegacyPluginConfig(legacyPath, legacyText, chosen, error)) {
		return false;
	}
	if (!ResolveSqlitePath(chosen, dataDir, error)) {
		return false;
	}
	warning = reason + " - using the database settings of '" + std::string(legacyPath) +
			  "'; move the connection to '" + pisex::ConfigDir() + "/databases.cfg' (section \"" +
			  kConnectionName + "\")";
	out = std::move(chosen);
	return true;
}

} // namespace perm_storage
