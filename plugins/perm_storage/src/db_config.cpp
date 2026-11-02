#include "db_config.hpp"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include <nlohmann/json.hpp>
#include <plg/plugin.hpp>

#include "kv1.hpp"

namespace perm_storage {
namespace {

// The plugin name is also the name of its databases.cfg section (PLUGIN_STANDARD.md section 4).
constexpr const char* kConnectionName = "perm_storage";
constexpr const char* kDatabasesCfgFile = "databases.cfg";
// The Metamod spelling the original read; the old path shim tried it relative to the server
// working directory before the plugin-specific locations.
constexpr const char* kMetamodDatabasesCfgPath = "addons/configs/databases.cfg";

// The defaults of the pre-standard loader, kept so a config.jsonc without the keys behaves exactly
// as it did (and an empty config still means a local SQLite file next to the plugin data).
constexpr const char* kDefaultSqlitePath = "perm_storage.sqlite3";
constexpr const char* kDefaultDatabase = "perm_storage";

// The directory mapping the removed path shim used to do: <GetConfigsDir()>/<plugin>. Empty
// when the plugify module has not filled the slot in (the unit tests provide it themselves).
std::string ConfigsDir() {
	return plg::GetConfigsDir ? std::string(plg::GetConfigsDir()) : std::string();
}

// The local path helper (the plugin has no path shim anymore). Accepts either separator, tolerates
// an empty component and never doubles the separator.
std::string JoinPath(std::string_view dir, std::string_view leaf) {
	if (dir.empty()) {
		return std::string(leaf);
	}
	if (leaf.empty()) {
		return std::string(dir);
	}
	std::string path(dir);
	if (path.back() != '/' && path.back() != '\\') {
		path.push_back('/');
	}
	while (!leaf.empty() && (leaf.front() == '/' || leaf.front() == '\\')) {
		leaf.remove_prefix(1);
	}
	path.append(leaf);
	return path;
}

// The "perm_storage" section of a parsed databases.cfg. The parser collapses a single top-level
// "Databases" block into the root node, so its children are the sections; a file that consists of
// just the "perm_storage" block is accepted too, and so is a file with several top-level blocks
// where one of them is "Databases".
const KvNode* FindSection(const KvNode& root) {
	if (const KvNode* section = root.Find(kConnectionName); section != nullptr) {
		return section;
	}
	if (const KvNode* databases = root.Find("Databases"); databases != nullptr) {
		if (const KvNode* section = databases->Find(kConnectionName); section != nullptr) {
			return section;
		}
	}
	if (!root.hasValue && kv1_detail::EqualNoCase(root.name, kConnectionName)) {
		return &root;
	}
	return nullptr;
}

// The "perm_storage" section of a databases.cfg tree: the same keys, defaults and fail-soft reading
// as plugins/vip/src/plugin.cpp:226, plugins/admin_system/src/plugin.cpp:256 and
// plugins/levels_ranks/src/plugin.cpp:279 (anything but "sqlite" means MySQL). Only a missing
// section is a reason to fall back - a section with a broken driver or host is used as given, like
// the reference ports do.
bool ReadDatabasesCfgSection(const KvNode& root, DatabaseConfig& out, std::string& reason) {
	const KvNode* section = FindSection(root);
	if (section == nullptr) {
		reason = std::string("databases.cfg has no '") + kConnectionName + "' section";
		return false;
	}

	plgdb::Config db;
	db.host = std::string(section->GetString("host", "127.0.0.1"));
	db.port = static_cast<uint16_t>(std::max(1, std::min(65535, section->GetInt("port", 3306))));
	db.user = std::string(section->GetString("user", ""));
	db.password = std::string(section->GetString("pass", ""));
	db.database = std::string(section->GetString("database", kDefaultDatabase));
	if (section->GetString("driver", "mysql") == "sqlite") {
		db.driver = plgdb::Driver::SQLite;
		db.sqlitePath = std::string(section->GetString("sqlite_path", kDefaultSqlitePath));
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
	config.db.sqlitePath = JoinPath(dataDir, config.db.sqlitePath);
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
	error.clear();

	const std::string configsDir = ConfigsDir();
	const std::string pluginConfigDir = JoinPath(configsDir, kConnectionName);
	// The old path shim's candidate list for "addons/configs/databases.cfg", in the same order:
	// the raw Metamod path (relative to the server working directory), then the plugin's own
	// config file, its translations directory and its data directory. The first file that exists
	// is authoritative: a malformed file or a missing section falls back to the legacy config of
	// this plugin instead of silently taking a different databases.cfg. The shared
	// <GetConfigsDir()>/databases.cfg is deliberately not a candidate - the old reader never
	// looked there, and it may belong to another plugin.
	const std::string pluginPath = JoinPath(pluginConfigDir, kDatabasesCfgFile);
	const std::string translationsPath = JoinPath(JoinPath(pluginConfigDir, "translations"), kDatabasesCfgFile);
	const std::string dataPath = JoinPath(JoinPath(dataDir, kConnectionName), kDatabasesCfgFile);
	const std::string candidates[] = {kMetamodDatabasesCfgPath, pluginPath, translationsPath, dataPath};

	std::string reason;
	DatabaseConfig chosen;
	std::string foundPath;
	std::string foundText;
	for (const std::string& path : candidates) {
		if (std::optional<std::string> text = ReadTextFile(path)) {
			foundPath = path;
			foundText = std::move(*text);
			break; // the first existing file is authoritative
		}
	}
	if (foundPath.empty()) {
		// The old reader's message when no candidate exists: the plugin's own file.
		reason = "cannot read '" + pluginPath + "'";
	} else {
		const std::optional<KvNode> tree = ParseKv1(foundText);
		if (!tree) {
			reason = "invalid KeyValues in '" + foundPath + "'";
		} else {
			std::string why;
			DatabaseConfig candidate;
			if (ReadDatabasesCfgSection(*tree, candidate, why)) {
				if (!ResolveSqlitePath(candidate, dataDir, error)) {
					return false;
				}
				out = std::move(candidate);
				return true;
			}
			reason = std::move(why);
		}
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
			  "'; move the connection to '" + pluginPath + "' (section \"" + kConnectionName + "\")";
	out = std::move(chosen);
	return true;
}

} // namespace perm_storage
