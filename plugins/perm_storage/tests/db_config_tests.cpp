// Tests for plugins/perm_storage/src/db_config.hpp - where the plugin takes its database connection
// from (PLUGIN_STANDARD.md section 4) and the transition-period fallback to config.jsonc - and for
// the self-contained KV1 reader (src/kv1.hpp) that replaces the first port's KeyValues shim.
//
// The search is the old pisexcompat one: the raw Metamod "addons/configs/databases.cfg" (relative to
// the working directory), <configs>/perm_storage/databases.cfg, its translations directory and
// <data>/perm_storage/databases.cfg. The first existing file is authoritative; the shared
// <configs>/databases.cfg decoy must never be read. <GetConfigsDir()> is pointed at a throwaway
// directory and the process runs from another throwaway directory, so no real file can leak in.
//
// Run by ../test.sh from build/tests. No shim library is linked here: the only plugify entry point
// used by src/db_config.cpp is GetConfigsDir(), served below.

#include <plg/plugin.hpp>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

#include <unistd.h>

#include "db_config.hpp"
#include "kv1.hpp"

// A real plugin gets these from PLUGIFY_PLUGIN (they are filled in by the plugify module). The
// test serves the config/data directories from the throwaway directory of the run and leaves the
// rest null.
namespace plg {
GetBaseDirFn GetBaseDir{nullptr};
GetExtensionsDirFn GetExtensionsDir{nullptr};
GetConfigsDirFn GetConfigsDir{nullptr};
GetDataDirFn GetDataDir{nullptr};
GetLogsDirFn GetLogsDir{nullptr};
GetCacheDirFn GetCacheDir{nullptr};
IsLoadedFn IsLoaded{nullptr};
LogFn Log{nullptr};
BeginZoneFn BeginZone{nullptr};
EndZoneFn EndZone{nullptr};
} // namespace plg

namespace {

std::string g_configsDir;
std::string g_dataDir;

plg::string TestConfigsDir() {
	return plg::string(g_configsDir.c_str());
}

plg::string TestDataDir() {
	return plg::string(g_dataDir.c_str());
}

int g_checks = 0;
int g_failures = 0;
std::string_view g_section = "?";

void Section(std::string_view name) {
	g_section = name;
	std::printf("-- %.*s\n", static_cast<int>(name.size()), name.data());
}

#define CHECK(expr)                                                                        \
	do {                                                                                   \
		++g_checks;                                                                        \
		if (!(expr)) {                                                                     \
			++g_failures;                                                                  \
			std::printf("FAIL [%.*s] %s:%d: %s\n", static_cast<int>(g_section.size()),     \
						g_section.data(), __FILE__, __LINE__, #expr);                      \
		}                                                                                  \
	} while (false)

#define CHECK_EQ_STR(actual, expected)                                                     \
	do {                                                                                   \
		++g_checks;                                                                        \
		const std::string a_ = std::string(actual);                                        \
		const std::string e_ = std::string(expected);                                      \
		if (a_ != e_) {                                                                    \
			++g_failures;                                                                  \
			std::printf("FAIL [%.*s] %s:%d: got \"%s\", expected \"%s\"\n",                \
						static_cast<int>(g_section.size()), g_section.data(), __FILE__,   \
						__LINE__, a_.c_str(), e_.c_str());                                 \
		}                                                                                  \
	} while (false)

#define CHECK_EQ_INT(actual, expected)                                                     \
	do {                                                                                   \
		++g_checks;                                                                        \
		const long long a_ = static_cast<long long>(actual);                               \
		const long long e_ = static_cast<long long>(expected);                             \
		if (a_ != e_) {                                                                    \
			++g_failures;                                                                  \
			std::printf("FAIL [%.*s] %s:%d: got %lld, expected %lld\n",                    \
						static_cast<int>(g_section.size()), g_section.data(), __FILE__,   \
						__LINE__, a_, e_);                                                 \
		}                                                                                  \
	} while (false)

#define CHECK_CONTAINS(haystack, needle)                                                   \
	do {                                                                                   \
		++g_checks;                                                                        \
		const std::string h_ = std::string(haystack);                                      \
		if (h_.find(needle) == std::string::npos) {                                        \
			++g_failures;                                                                  \
			std::printf("FAIL [%.*s] %s:%d: \"%s\" does not contain \"%s\"\n",             \
						static_cast<int>(g_section.size()), g_section.data(), __FILE__,   \
						__LINE__, h_.c_str(), std::string(needle).c_str());                \
		}                                                                                  \
	} while (false)

// --- helpers -----------------------------------------------------------------

constexpr std::string_view kLegacyConfig = R"({
	"driver": "mysql",
	"host": "legacy.example.com",
	"port": 3308,
	"user": "legacy_user",
	"password": "legacy_pass",
	"database": "legacy_db"
})";

std::string LegacyPath() {
	return (std::filesystem::path(g_configsDir) / "perm_storage" / "config.jsonc").string();
}

// The decoy the old reader ignored: <GetConfigsDir()>/databases.cfg.
std::string RootDatabasesCfgPath() {
	return (std::filesystem::path(g_configsDir) / "databases.cfg").string();
}

// <GetConfigsDir()>/perm_storage/databases.cfg (the standard location).
std::string DatabasesCfgPath() {
	return (std::filesystem::path(g_configsDir) / "perm_storage" / "databases.cfg").string();
}

// <GetConfigsDir()>/perm_storage/translations/databases.cfg (old ResolvePath fallback).
std::string TranslationsDatabasesCfgPath() {
	return (std::filesystem::path(g_configsDir) / "perm_storage" / "translations" / "databases.cfg").string();
}

// <GetDataDir()>/perm_storage/databases.cfg (old ResolvePath fallback).
std::string DataDatabasesCfgPath() {
	return (std::filesystem::path(g_dataDir) / "perm_storage" / "databases.cfg").string();
}

// "addons/configs/databases.cfg" relative to the working directory (old ResolvePath first candidate).
std::string RawDatabasesCfgPath() {
	return (std::filesystem::current_path() / "addons" / "configs" / "databases.cfg").string();
}

void WriteFile(const std::string& path, std::string_view text) {
	std::error_code ec;
	std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
	std::ofstream out(path, std::ios::binary | std::ios::trunc);
	out << text;
}

void WriteDatabasesCfg(std::string_view text) {
	WriteFile(DatabasesCfgPath(), text);
}

void WriteTranslationsDatabasesCfg(std::string_view text) {
	WriteFile(TranslationsDatabasesCfgPath(), text);
}

void WriteDataDatabasesCfg(std::string_view text) {
	WriteFile(DataDatabasesCfgPath(), text);
}

void WriteRootDatabasesCfg(std::string_view text) {
	WriteFile(RootDatabasesCfgPath(), text);
}

void WriteRawDatabasesCfg(std::string_view text) {
	WriteFile(RawDatabasesCfgPath(), text);
}

void RemoveFile(const std::string& path) {
	std::error_code ec;
	std::filesystem::remove(path, ec);
}

// Every database test starts from a working directory and config/data roots with no databases.cfg.
void RemoveDatabasesCfg() {
	RemoveFile(RawDatabasesCfgPath());
	RemoveFile(DatabasesCfgPath());
	RemoveFile(TranslationsDatabasesCfgPath());
	RemoveFile(DataDatabasesCfgPath());
	RemoveFile(RootDatabasesCfgPath());
}

std::string OneSectionCfg(std::string_view host, int port) {
	return std::string("\"Databases\" { \"perm_storage\" { \"driver\" \"mysql\" \"host\" \"") +
		   std::string(host) + "\" \"port\" \"" + std::to_string(port) + "\" } }";
}

// --- KV1 reader --------------------------------------------------------------

void TestKv1Parser() {
	Section("KV1 parser: grammar, comments, escapes and malformed input");

	constexpr std::string_view kKvText = R"kv(
		// databases.cfg is plain Valve KeyValues.
		"Databases"
		{
			"PERM_STORAGE"
			{
				"driver"	"sqlite" // trailing comment
				"host"		unquoted.example.com
				"port"		"3307"
				"path"		"C:\\data\\perm.sqlite3"
				"quote"		"a\"b"
				"newline"	"a\nb"
				empty		""
				/* block comment */
				"both"		"value"
				{
					"child"	"42"
				}
			}
		}
	)kv";

	const std::optional<perm_storage::KvNode> root = perm_storage::ParseKv1(kKvText);
	CHECK(root.has_value());
	CHECK_EQ_STR(root ? root->name : std::string(), "Databases");
	const perm_storage::KvNode* section = root ? root->Find("perm_storage") : nullptr;
	CHECK(section != nullptr);
	if (section != nullptr) {
		CHECK_EQ_STR(section->GetString("DRIVER"), "sqlite");
		CHECK_EQ_STR(section->GetString("host"), "unquoted.example.com");
		CHECK_EQ_INT(section->GetInt("port", 3306), 3307);
		CHECK_EQ_STR(section->GetString("path"), "C:\\data\\perm.sqlite3");
		CHECK_EQ_STR(section->GetString("quote"), "a\"b");
		CHECK_EQ_STR(section->GetString("newline"), "a\nb");
		CHECK(section->Find("empty") != nullptr);
		CHECK(section->GetString("empty").empty()); // an empty value is still a value
		CHECK_EQ_STR(section->GetString("missing", "fallback"), "fallback");
		CHECK_EQ_INT(section->GetInt("missing", 9), 9);
		CHECK_EQ_INT(section->GetInt("empty", 9), 9);
		const perm_storage::KvNode* both = section->Find("both");
		CHECK(both != nullptr);
		if (both != nullptr) {
			CHECK_EQ_STR(both->GetString(""), "value"); // a key may carry a value and a block
			CHECK_EQ_INT(both->GetInt("child", 0), 42);
		}
	}

	// A single top-level block is the root (Valve's loader semantics).
	const std::optional<perm_storage::KvNode> direct =
		perm_storage::ParseKv1(R"kv("perm_storage" { "driver" "sqlite" })kv");
	CHECK(direct.has_value());
	CHECK(direct && direct->name == "perm_storage");
	CHECK(direct && direct->Find("driver") != nullptr);

	// #include / #define lines are skipped, not included.
	const std::optional<perm_storage::KvNode> includeSkipped = perm_storage::ParseKv1(
		"#include \"other.cfg\"\n\"Databases\" { \"perm_storage\" { \"host\" \"h\" } }");
	CHECK(includeSkipped.has_value());
	CHECK(includeSkipped && includeSkipped->Find("perm_storage") != nullptr);

	// A UTF-8 BOM from a Windows editor is not part of the first key.
	const std::optional<perm_storage::KvNode> bom = perm_storage::ParseKv1(
		"\xEF\xBB\xBF"
		"\"Databases\" { \"perm_storage\" { \"host\" \"h\" } }");
	CHECK(bom.has_value());
	CHECK(bom && bom->Find("perm_storage") != nullptr);

	// A half-read tree is never returned.
	CHECK(!perm_storage::ParseKv1(R"kv("Databases" { "perm_storage" { "host" "x" })kv").has_value());
	CHECK(!perm_storage::ParseKv1(R"kv("Databases" { "perm_storage" { "host" "unterminated)kv").has_value());
	CHECK(!perm_storage::ParseKv1(R"kv("Databases" { } })kv").has_value());
	CHECK(!perm_storage::ParseKv1(R"kv("Databases" { "perm_storage" { )kv").has_value());
}

// --- search order and authority ----------------------------------------------

void TestPluginDatabasesCfgWins() {
	Section("plugin databases.cfg wins over the translations/data/root decoys");

	RemoveDatabasesCfg();
	WriteDatabasesCfg(R"("Databases"
{
	"perm_storage"
	{
		"driver"	"mysql"
		"host"		"db.example.com"
		"port"		"3307"
		"user"		"ps_user"
		"pass"		"ps_pass"
		"database"	"ps_db"
	}
}
)");
	WriteTranslationsDatabasesCfg(OneSectionCfg("translations-decoy.example.com", 3403));
	WriteDataDatabasesCfg(OneSectionCfg("data-decoy.example.com", 3404));
	WriteRootDatabasesCfg(OneSectionCfg("root-decoy.example.com", 3405));

	perm_storage::DatabaseConfig config;
	std::string warning;
	std::string error;
	CHECK(perm_storage::LoadDatabaseConfig(LegacyPath(), kLegacyConfig, g_dataDir, config, warning, error));
	CHECK_EQ_STR(error, "");
	CHECK_EQ_STR(warning, "");
	CHECK(config.source == perm_storage::DatabaseSource::DatabasesCfg);
	CHECK(config.db.driver == plgdb::Driver::MySQL);
	CHECK_EQ_STR(config.db.host, "db.example.com");
	CHECK_EQ_INT(config.db.port, 3307);
	CHECK_EQ_STR(config.db.user, "ps_user");
	CHECK_EQ_STR(config.db.password, "ps_pass");
	CHECK_EQ_STR(config.db.database, "ps_db");
}

void TestRootDatabasesCfgIgnored() {
	Section("shared <configs>/databases.cfg is never read (old decoy semantics)");

	RemoveDatabasesCfg();
	// A perfectly valid section in the shared root file: the old reader ignored it, and reading it
	// here would silently point the plugin at another plugin's database.
	WriteRootDatabasesCfg(OneSectionCfg("root-decoy.example.com", 3405));

	perm_storage::DatabaseConfig config;
	std::string warning;
	std::string error;
	CHECK(perm_storage::LoadDatabaseConfig(LegacyPath(), kLegacyConfig, g_dataDir, config, warning, error));
	CHECK_EQ_STR(error, "");
	CHECK(config.source == perm_storage::DatabaseSource::LegacyConfig);
	CHECK_EQ_STR(config.db.host, "legacy.example.com");
	CHECK_EQ_INT(config.db.port, 3308);
	CHECK_CONTAINS(warning, "cannot read");
	CHECK_CONTAINS(warning, "databases.cfg");
	CHECK_CONTAINS(warning, "move the connection");
	CHECK(warning.find("root-decoy") == std::string::npos);
}

void TestSearchOrder() {
	Section("search order: raw cwd, plugin cfg, translations, data");

	RemoveDatabasesCfg();
	WriteRawDatabasesCfg(OneSectionCfg("raw.example.com", 3401));
	WriteDatabasesCfg(OneSectionCfg("plugin.example.com", 3402));
	WriteTranslationsDatabasesCfg(OneSectionCfg("translations.example.com", 3403));
	WriteDataDatabasesCfg(OneSectionCfg("data.example.com", 3404));
	WriteRootDatabasesCfg(OneSectionCfg("root-decoy.example.com", 3405));

	perm_storage::DatabaseConfig config;
	std::string warning;
	std::string error;

	// 1. The raw Metamod path relative to the working directory is the first candidate.
	CHECK(perm_storage::LoadDatabaseConfig(LegacyPath(), kLegacyConfig, g_dataDir, config, warning, error));
	CHECK_EQ_STR(error, "");
	CHECK_EQ_STR(warning, "");
	CHECK(config.source == perm_storage::DatabaseSource::DatabasesCfg);
	CHECK_EQ_STR(config.db.host, "raw.example.com");
	CHECK_EQ_INT(config.db.port, 3401);

	// 2. Without it, the plugin's own config file wins over translations/data.
	RemoveFile(RawDatabasesCfgPath());
	CHECK(perm_storage::LoadDatabaseConfig(LegacyPath(), kLegacyConfig, g_dataDir, config, warning, error));
	CHECK_EQ_STR(warning, "");
	CHECK_EQ_STR(config.db.host, "plugin.example.com");
	CHECK_EQ_INT(config.db.port, 3402);

	// 3. ... then its translations directory ...
	RemoveFile(DatabasesCfgPath());
	CHECK(perm_storage::LoadDatabaseConfig(LegacyPath(), kLegacyConfig, g_dataDir, config, warning, error));
	CHECK_EQ_STR(warning, "");
	CHECK_EQ_STR(config.db.host, "translations.example.com");
	CHECK_EQ_INT(config.db.port, 3403);

	// 4. ... and, last, the data directory. The shared root decoy is still ignored.
	RemoveFile(TranslationsDatabasesCfgPath());
	CHECK(perm_storage::LoadDatabaseConfig(LegacyPath(), kLegacyConfig, g_dataDir, config, warning, error));
	CHECK_EQ_STR(warning, "");
	CHECK_EQ_STR(config.db.host, "data.example.com");
	CHECK_EQ_INT(config.db.port, 3404);
}

void TestBrokenRawFileIsAuthoritative() {
	Section("the raw cwd file is authoritative even when broken (no fall-through)");

	RemoveDatabasesCfg();
	WriteRawDatabasesCfg(R"kv("Databases" { "perm_storage" { "host" "unterminated )kv");
	WriteDatabasesCfg(OneSectionCfg("plugin.example.com", 3402));

	perm_storage::DatabaseConfig config;
	std::string warning;
	std::string error;
	CHECK(perm_storage::LoadDatabaseConfig(LegacyPath(), kLegacyConfig, g_dataDir, config, warning, error));
	CHECK_EQ_STR(error, "");
	CHECK(config.source == perm_storage::DatabaseSource::LegacyConfig);
	CHECK_EQ_STR(config.db.host, "legacy.example.com");
	CHECK_CONTAINS(warning, "invalid KeyValues");
	CHECK_CONTAINS(warning, "addons/configs/databases.cfg");

	// The same rule for a raw file without the "perm_storage" section.
	WriteRawDatabasesCfg(R"("Databases" { "admin_system" { "host" "other.example.com" } })");
	CHECK(perm_storage::LoadDatabaseConfig(LegacyPath(), kLegacyConfig, g_dataDir, config, warning, error));
	CHECK_EQ_STR(error, "");
	CHECK(config.source == perm_storage::DatabaseSource::LegacyConfig);
	CHECK_EQ_STR(config.db.host, "legacy.example.com");
	CHECK_CONTAINS(warning, "'perm_storage' section");
}

void TestFirstFoundFileIsAuthoritative() {
	Section("a broken or section-less first file falls back to legacy, never to the next file");

	RemoveDatabasesCfg();
	WriteDatabasesCfg(R"kv("Databases" { "perm_storage" { "host" "unterminated )kv");
	WriteTranslationsDatabasesCfg(OneSectionCfg("translations.example.com", 3403));
	WriteDataDatabasesCfg(OneSectionCfg("data.example.com", 3404));

	perm_storage::DatabaseConfig config;
	std::string warning;
	std::string error;
	CHECK(perm_storage::LoadDatabaseConfig(LegacyPath(), kLegacyConfig, g_dataDir, config, warning, error));
	CHECK_EQ_STR(error, "");
	CHECK(config.source == perm_storage::DatabaseSource::LegacyConfig);
	CHECK_EQ_STR(config.db.host, "legacy.example.com");
	CHECK_CONTAINS(warning, "invalid KeyValues");
	CHECK_CONTAINS(warning, "perm_storage/databases.cfg");

	// Valid KeyValues but no "perm_storage" section: the same rule, not a silent substitution.
	WriteDatabasesCfg(R"("Databases"
{
	"admin_system"
	{
		"host"		"other.example.com"
	}
}
)");
	CHECK(perm_storage::LoadDatabaseConfig(LegacyPath(), kLegacyConfig, g_dataDir, config, warning, error));
	CHECK_EQ_STR(error, "");
	CHECK(config.source == perm_storage::DatabaseSource::LegacyConfig);
	CHECK_EQ_STR(config.db.host, "legacy.example.com");
	CHECK_CONTAINS(warning, "'perm_storage' section");
	CHECK_CONTAINS(warning, "databases.cfg");
}

void TestMissingDatabasesCfgFallsBack() {
	Section("no databases.cfg in any candidate -> config.jsonc + one warning");

	RemoveDatabasesCfg();

	perm_storage::DatabaseConfig config;
	std::string warning;
	std::string error;
	CHECK(perm_storage::LoadDatabaseConfig(LegacyPath(), kLegacyConfig, g_dataDir, config, warning, error));
	CHECK_EQ_STR(error, "");
	CHECK(config.source == perm_storage::DatabaseSource::LegacyConfig);
	CHECK(config.db.driver == plgdb::Driver::MySQL);
	CHECK_EQ_STR(config.db.host, "legacy.example.com");
	CHECK_EQ_INT(config.db.port, 3308);
	CHECK_EQ_STR(config.db.user, "legacy_user");
	CHECK_EQ_STR(config.db.password, "legacy_pass");
	CHECK_EQ_STR(config.db.database, "legacy_db");
	CHECK_CONTAINS(warning, "cannot read");
	CHECK_CONTAINS(warning, "databases.cfg");
	CHECK_CONTAINS(warning, "perm_storage"); // the section the connection has to move into
	CHECK_CONTAINS(warning, "move the connection");
	CHECK_CONTAINS(warning, "perm_storage/databases.cfg"); // the exact destination
}

void TestDatabasesCfgWithoutWrapper() {
	Section("a file with only the 'perm_storage' block is accepted");

	RemoveDatabasesCfg();
	WriteDatabasesCfg(
		R"("perm_storage" { "driver" "sqlite" "sqlite_path" ":memory:" })");

	perm_storage::DatabaseConfig config;
	std::string warning;
	std::string error;
	CHECK(perm_storage::LoadDatabaseConfig(LegacyPath(), kLegacyConfig, g_dataDir, config, warning, error));
	CHECK_EQ_STR(error, "");
	CHECK_EQ_STR(warning, "");
	CHECK(config.source == perm_storage::DatabaseSource::DatabasesCfg);
	CHECK(config.db.driver == plgdb::Driver::SQLite);
	CHECK_EQ_STR(config.db.sqlitePath, ":memory:");
}

// --- sqlite paths ------------------------------------------------------------

void TestSqliteSection() {
	Section("sqlite connection: the path is resolved against the data directory");

	RemoveDatabasesCfg();
	WriteDatabasesCfg(R"("Databases"
{
	"perm_storage"
	{
		"driver"	"sqlite"
		"sqlite_path"	"nested/ps.sqlite3"
	}
}
)");

	perm_storage::DatabaseConfig config;
	std::string warning;
	std::string error;
	CHECK(perm_storage::LoadDatabaseConfig(LegacyPath(), kLegacyConfig, g_dataDir, config, warning, error));
	CHECK_EQ_STR(error, "");
	CHECK_EQ_STR(warning, "");
	CHECK(config.source == perm_storage::DatabaseSource::DatabasesCfg);
	CHECK(config.db.driver == plgdb::Driver::SQLite);
	CHECK_EQ_STR(config.db.sqlitePath, (std::filesystem::path(g_dataDir) / "nested/ps.sqlite3").string());
	CHECK(std::filesystem::is_directory(std::filesystem::path(g_dataDir) / "nested"));

	// ":memory:" is kept as given.
	WriteDatabasesCfg(R"("Databases" { "perm_storage" { "driver" "sqlite" "sqlite_path" ":memory:" } })");
	CHECK(perm_storage::LoadDatabaseConfig(LegacyPath(), kLegacyConfig, g_dataDir, config, warning, error));
	CHECK_EQ_STR(config.db.sqlitePath, ":memory:");

	// An absolute path is kept too (nothing is created or rewritten).
	const std::string absolute = (std::filesystem::path(g_configsDir) / "absolute.sqlite3").string();
	WriteDatabasesCfg("\"Databases\" { \"perm_storage\" { \"driver\" \"sqlite\" \"sqlite_path\" \"" +
					  absolute + "\" } }");
	CHECK(perm_storage::LoadDatabaseConfig(LegacyPath(), kLegacyConfig, g_dataDir, config, warning, error));
	CHECK_EQ_STR(config.db.sqlitePath, absolute);

	// An empty path is a hard error, as before the standard.
	WriteDatabasesCfg(R"("Databases" { "perm_storage" { "driver" "sqlite" "sqlite_path" "" } })");
	CHECK(!perm_storage::LoadDatabaseConfig(LegacyPath(), kLegacyConfig, g_dataDir, config, warning, error));
	CHECK_CONTAINS(error, "'sqlite_path' must not be empty");
}

void TestRelativeSqliteNeedsDataDir() {
	Section("a relative sqlite path without a data directory is a hard error");

	RemoveDatabasesCfg();
	WriteDatabasesCfg(R"("Databases" { "perm_storage" { "driver" "sqlite" "sqlite_path" "ps.sqlite3" } })");

	perm_storage::DatabaseConfig config;
	std::string warning;
	std::string error;
	CHECK(!perm_storage::LoadDatabaseConfig(LegacyPath(), kLegacyConfig, /*dataDir=*/"", config, warning, error));
	CHECK_CONTAINS(error, "data directory");
}

// --- section values ----------------------------------------------------------

void TestSectionValues() {
	Section("defaults, pass/database keys and the port clamp");

	RemoveDatabasesCfg();
	auto load = [&](std::string_view body) {
		WriteDatabasesCfg(std::string("\"Databases\" { \"perm_storage\" { ") + std::string(body) + " } }");
		perm_storage::DatabaseConfig config;
		std::string warning;
		std::string error;
		const bool ok =
			perm_storage::LoadDatabaseConfig(LegacyPath(), kLegacyConfig, g_dataDir, config, warning, error);
		CHECK(ok);
		CHECK_EQ_STR(error, "");
		return config;
	};

	// Missing keys keep the historical defaults (MySQL on 127.0.0.1:3306, section name as database).
	const perm_storage::DatabaseConfig defaults = load("");
	CHECK(defaults.db.driver == plgdb::Driver::MySQL);
	CHECK_EQ_STR(defaults.db.host, "127.0.0.1");
	CHECK_EQ_INT(defaults.db.port, 3306);
	CHECK_EQ_STR(defaults.db.user, "");
	CHECK_EQ_STR(defaults.db.password, "");
	CHECK_EQ_STR(defaults.db.database, "perm_storage");
	CHECK_EQ_STR(defaults.db.charset, "utf8mb4");

	const perm_storage::DatabaseConfig values =
		load("\"host\" \"db.example.com\" \"port\" \"3307\" \"user\" \"u\" \"pass\" \"p\" "
			 "\"database\" \"d\"");
	CHECK_EQ_STR(values.db.host, "db.example.com");
	CHECK_EQ_INT(values.db.port, 3307);
	CHECK_EQ_STR(values.db.user, "u");
	CHECK_EQ_STR(values.db.password, "p"); // the databases.cfg spelling is "pass"
	CHECK_EQ_STR(values.db.database, "d");

	// The reference ports clamp the port instead of failing.
	CHECK_EQ_INT(load("\"port\" \"0\"").db.port, 1);
	CHECK_EQ_INT(load("\"port\" \"70000\"").db.port, 65535);
	CHECK_EQ_INT(load("\"port\" \"not-a-number\"").db.port, 1);
}

// --- legacy config -----------------------------------------------------------

void TestLegacyDefaults() {
	Section("the shipped config.jsonc (no database keys) still means SQLite");

	RemoveDatabasesCfg();

	// The file package.sh ships: a comment pointing at databases.cfg, no keys at all.
	constexpr std::string_view kShippedConfig = R"({
	// The database connection of this plugin lives in databases.cfg, next to this file:
	// configs/perm_storage/databases.cfg, section "perm_storage"
	// (docs/PLUGIN_STANDARD.md section 4). While that section does not exist, the plugin still
	// reads the legacy connection keys of this file (driver, sqlite_path, host, port, user,
	// password, database) and logs one warning.
})";

	perm_storage::DatabaseConfig config;
	std::string warning;
	std::string error;
	CHECK(perm_storage::LoadDatabaseConfig(LegacyPath(), kShippedConfig, g_dataDir, config, warning, error));
	CHECK_EQ_STR(error, "");
	CHECK(config.source == perm_storage::DatabaseSource::LegacyConfig);
	CHECK(config.db.driver == plgdb::Driver::SQLite);
	CHECK_EQ_STR(config.db.sqlitePath, (std::filesystem::path(g_dataDir) / "perm_storage.sqlite3").string());
	CHECK(!warning.empty());
}

void TestBrokenLegacyConfigFails() {
	Section("a broken config.jsonc is still a hard error");

	RemoveDatabasesCfg();

	perm_storage::DatabaseConfig config;
	std::string warning;
	std::string error;
	CHECK(!perm_storage::LoadDatabaseConfig(LegacyPath(), "{ \"driver\": ", g_dataDir, config, warning, error));
	CHECK_CONTAINS(error, "config parse error");
	CHECK_CONTAINS(error, "config.jsonc");

	// An unknown driver in the legacy file is an error too, as before the standard.
	CHECK(!perm_storage::LoadDatabaseConfig(LegacyPath(), R"({ "driver": "postgres" })", g_dataDir, config, warning, error));
	CHECK_CONTAINS(error, "unknown 'driver'");
}

void TestMissingConfigsDirFallsBack() {
	Section("no plugify config directory -> no candidate found, the legacy config still works");

	RemoveDatabasesCfg();
	const std::string savedConfigsDir = g_configsDir;
	g_configsDir.clear();

	perm_storage::DatabaseConfig config;
	std::string warning;
	std::string error;
	CHECK(perm_storage::LoadDatabaseConfig(LegacyPath(), kLegacyConfig, g_dataDir, config, warning, error));
	CHECK_EQ_STR(error, "");
	CHECK(config.source == perm_storage::DatabaseSource::LegacyConfig);
	CHECK_EQ_STR(config.db.host, "legacy.example.com");
	CHECK_CONTAINS(warning, "cannot read");
	CHECK_CONTAINS(warning, "perm_storage/databases.cfg");

	g_configsDir = savedConfigsDir;
}

} // namespace

int main() {
	const std::filesystem::path tempRoot =
		std::filesystem::temp_directory_path() / ("perm_storage_dbconfig_tests_" + std::to_string(::getpid()));
	std::error_code ec;
	std::filesystem::remove_all(tempRoot, ec);
	std::filesystem::create_directories(tempRoot / "perm_storage", ec);
	std::filesystem::create_directories(tempRoot / "data", ec);
	std::filesystem::create_directories(tempRoot / "cwd", ec);
	if (!std::filesystem::is_directory(tempRoot / "perm_storage") ||
	    !std::filesystem::is_directory(tempRoot / "data") ||
	    !std::filesystem::is_directory(tempRoot / "cwd")) {
		std::printf("FATAL: cannot create the test directory %s\n", tempRoot.c_str());
		return 1;
	}
	g_configsDir = tempRoot.string();
	g_dataDir = (tempRoot / "data").string();
	plg::GetConfigsDir = &TestConfigsDir;
	plg::GetDataDir = &TestDataDir;
	// The raw "addons/configs/databases.cfg" candidate is relative to the working directory: run
	// from an empty throwaway directory so only files this test creates can be found there.
	std::filesystem::current_path(tempRoot / "cwd", ec);
	if (ec) {
		std::printf("FATAL: cannot enter the test directory %s: %s\n", (tempRoot / "cwd").c_str(), ec.message().c_str());
		return 1;
	}

	std::printf("perm_storage db_config tests (%s)\n", tempRoot.c_str());

	TestKv1Parser();
	TestPluginDatabasesCfgWins();
	TestRootDatabasesCfgIgnored();
	TestSearchOrder();
	TestBrokenRawFileIsAuthoritative();
	TestFirstFoundFileIsAuthoritative();
	TestMissingDatabasesCfgFallsBack();
	TestDatabasesCfgWithoutWrapper();
	TestSqliteSection();
	TestRelativeSqliteNeedsDataDir();
	TestSectionValues();
	TestLegacyDefaults();
	TestBrokenLegacyConfigFails();
	TestMissingConfigsDirFallsBack();

	std::filesystem::current_path(std::filesystem::temp_directory_path(), ec);
	std::filesystem::remove_all(tempRoot, ec);
	std::printf("%d checks, %d failures\n", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
