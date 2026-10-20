// Plugify glue for the "perm_storage" plugin: config loading, the permissions:: API calls
// driven by the load plans of perm_storage::PermStore, the *Storage listener bodies and
// the Poll()/retry pump.
//
// Nothing here blocks on the database on the game thread: the only synchronous statements
// are the schema creation and the group snapshot read at plugin start, and their re-run after
// a degraded start goes through the worker (see RunDatabaseRecovery).
//
// Flow (the storage/connector contract of the permissions core):
//   * OnPluginStart: EnsureSchema -> LoadGroupsSync -> permissions::CreateGroup for every
//     group (parents first, own pluginID + dontBroadcast) -> register the listeners.
//   * A connector calls permissions::LoadUser(connectorID, steamid, name, offline, ..) which
//     fans out OnUserRequest. This plugin answers "not ready" (returns false -> DBNotReady)
//     and loads the user asynchronously; when the plan lands, ApplyUserPlan() replays it
//     through the API with the own pluginID and re-issues LoadUser(ownID, .., false), which
//     is what broadcasts OnUserLoaded to the connector.
//   * Every storage event of another pluginID is written back with a fire-and-forget
//     statement; events carrying the own pluginID are the echoes of the replays above.
//   * A database that was unreachable at start (or a first group read that failed) is retried
//     from OnPluginUpdate with a growing backoff; until the schema and the group snapshot are
//     in place the plugin answers DBNotReady and persists nothing.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <permissions/permissions.hpp>
#include <plg/any.hpp>
#include <plg/plugin.hpp>
#include <plg/string.hpp>
#include <plugin_export.h>

#include <plgdb/db.hpp>

#include "db_config.hpp"
#include "perm_storage_core.hpp"

namespace {

// The database connection of the plugin lives in databases.cfg (the standard source, see
// docs/PLUGIN_STANDARD.md section 4), so this file has no keys of its own anymore; the plugin
// still creates it on first start as a pointer to that file.
constexpr std::string_view kDefaultConfig = R"({
	// The database connection of this plugin lives in databases.cfg, next to this file:
	// configs/perm_storage/databases.cfg, section "perm_storage"
	// (docs/PLUGIN_STANDARD.md section 4). While that section does not exist, the plugin still
	// reads the legacy connection keys of this file (driver, sqlite_path, host, port, user,
	// password, database) and logs one warning.
})";

// Degraded-start recovery (see RunDatabaseRecovery): the first re-attempt runs 5 s after the
// failure, every later one doubles the wait up to 60 s, so a database that comes up after the
// server (or a table that is created later) needs no restart.
constexpr std::chrono::seconds kRecoveryInitialDelay{5};
constexpr std::chrono::seconds kRecoveryMaxDelay{60};

void LogMessage(std::string_view message, plg::Severity severity) {
	if (plg::Log) {
		plg::Log(message, severity, plg::source_location::current());
	}
}

void LogInfo(std::string_view message) {
	LogMessage(message, plg::Severity::Info);
}

void LogWarning(std::string_view message) {
	LogMessage(message, plg::Severity::Warning);
}

void LogError(std::string_view message) {
	LogMessage(message, plg::Severity::Error);
}

std::string JoinPath(std::string_view dir, std::string_view leaf) {
	if (dir.empty()) {
		return std::string(leaf);
	}
	std::string path(dir);
	if (path.back() != '/' && path.back() != '\\') {
		path.push_back('/');
	}
	path.append(leaf);
	return path;
}

std::string StatusText(permissions::Status status) {
	return std::to_string(static_cast<int32_t>(status));
}

std::string ToStd(const plg::string& text) {
	return std::string(static_cast<std::string_view>(text));
}

std::string_view ToView(const plg::string& text) {
	return static_cast<std::string_view>(text);
}

// --- plg::any <-> stored value -----------------------------------------------
// The persisted subset: bool, integers (widened to int64), float/double and string.
// Anything else is rejected with a message (the storage cannot represent it).
std::optional<perm_storage::Value> AnyToValue(const plg::any& any) {
	if (const auto* value = plg::get_if<bool>(&any)) {
		return perm_storage::Value::MakeBool(*value);
	}
	if (const auto* value = plg::get_if<int64_t>(&any)) {
		return perm_storage::Value::MakeInt(*value);
	}
	if (const auto* value = plg::get_if<int32_t>(&any)) {
		return perm_storage::Value::MakeInt(*value);
	}
	if (const auto* value = plg::get_if<int16_t>(&any)) {
		return perm_storage::Value::MakeInt(*value);
	}
	if (const auto* value = plg::get_if<int8_t>(&any)) {
		return perm_storage::Value::MakeInt(*value);
	}
	if (const auto* value = plg::get_if<uint64_t>(&any)) {
		return perm_storage::Value::MakeInt(static_cast<int64_t>(*value));
	}
	if (const auto* value = plg::get_if<uint32_t>(&any)) {
		return perm_storage::Value::MakeInt(*value);
	}
	if (const auto* value = plg::get_if<uint16_t>(&any)) {
		return perm_storage::Value::MakeInt(*value);
	}
	if (const auto* value = plg::get_if<uint8_t>(&any)) {
		return perm_storage::Value::MakeInt(*value);
	}
	if (const auto* value = plg::get_if<double>(&any)) {
		return perm_storage::Value::MakeDouble(*value);
	}
	if (const auto* value = plg::get_if<float>(&any)) {
		return perm_storage::Value::MakeDouble(*value);
	}
	if (const auto* value = plg::get_if<plg::string>(&any)) {
		return perm_storage::Value::MakeString(std::string(*value));
	}
	return std::nullopt;
}

plg::any ValueToAny(const perm_storage::Value& value) {
	switch (value.kind) {
	case perm_storage::Value::Kind::Bool: return plg::any(value.boolean);
	case perm_storage::Value::Kind::Int: return plg::any(value.integer);
	case perm_storage::Value::Kind::Double: return plg::any(value.real);
	case perm_storage::Value::Kind::String: return plg::any(plg::string(value.text));
	}
	return plg::any();
}

perm_storage::PermChange ToPermChange(permissions::Action action, std::string perm,
                                      permissions::Status newState, int64_t newTimestamp) {
	perm_storage::PermChange change;
	change.perm = std::move(perm);
	change.denied = newState == permissions::Status::Disallow;
	change.expiresAt = newTimestamp;
	switch (action) {
	case permissions::Action::Remove: change.action = perm_storage::PermAction::Remove; break;
	case permissions::Action::Replace: change.action = perm_storage::PermAction::Replace; break;
	case permissions::Action::ReplaceToWC: change.action = perm_storage::PermAction::ReplaceToWildcard; break;
	case permissions::Action::Add: change.action = perm_storage::PermAction::Add; break;
	}
	return change;
}

// --- plugin ------------------------------------------------------------------
class PermStoragePlugin final : public plg::Plugin {
public:
	plg::PluginResult OnPluginStart() override;
	plg::PluginResult OnPluginUpdate(std::chrono::milliseconds dt) override;
	plg::PluginResult OnPluginEnd() override;

	// Listener bodies (game thread, invoked by the permissions core).
	bool HandleUserRequest(int64_t pluginID, uint64_t targetID, const plg::string& username, bool offline);
	bool HandleGroupsLoad(int64_t pluginID);
	void HandleUserDelete(int64_t pluginID, uint64_t targetID);
	bool HandleUserImmunityStorage(int64_t pluginID, uint64_t targetID, int32_t immunity);
	bool HandleUserPermissionStorage(int64_t pluginID, permissions::Action action, uint64_t targetID,
	                                 const plg::string& perm, permissions::Status oldState,
	                                 permissions::Status newState, int64_t oldTimestamp, int64_t newTimestamp);
	bool HandleUserCookieStorage(int64_t pluginID, uint64_t targetID, const plg::string& name,
	                             const plg::any& cookie);
	bool HandleUserGroupStorage(int64_t pluginID, permissions::Action action, uint64_t targetID,
	                            const plg::string& group, int64_t oldTimestamp, int64_t newTimestamp);
	bool HandleGroupCreateStorage(int64_t pluginID, const plg::string& name,
	                              const plg::vector<plg::string>& perms, int32_t priority,
	                              const plg::string& parent);
	bool HandleGroupDeleteStorage(int64_t pluginID, const plg::string& name);
	bool HandleGroupPermissionStorage(int64_t pluginID, permissions::Action action, const plg::string& groupName,
	                                  const plg::string& perm, permissions::Status oldState,
	                                  permissions::Status newState);
	bool HandleGroupOptionStorage(int64_t pluginID, const plg::string& groupName, const plg::string& optionName,
	                              const plg::any& value);
	bool HandleSetParentStorage(int64_t pluginID, const plg::string& childName, const plg::string& parentName);

private:
	bool LoadConfig(std::string& error);
	bool CreateDatabase(std::string& error);
	// True when every permissions binding this plugin calls (methods and listeners) is filled
	// in by the plugify module; nothing may call into the core before this passed.
	static bool PermissionsBindingsAvailable();
	bool RegisterListeners(std::string& error);
	void UnregisterListeners();
	// Creates every group of the snapshot that is missing in the core (parents first).
	void ApplyGroupsToCore();
	void ApplyUserPlan(const perm_storage::UserPlan& plan);
	// Logs the error and keeps the plugin loaded in degraded mode (nothing is persisted).
	plg::PluginResult Degraded(std::string_view what, std::string_view error);
	// Starts the repeated schema + group reload after a degraded start (no-op without a store).
	void ArmDatabaseRecovery();
	// One asynchronous recovery attempt once its backoff expired; pumped by OnPluginUpdate.
	void RunDatabaseRecovery();

	plgdb::Config m_dbConfig;
	std::unique_ptr<plgdb::Database> m_db;
	std::unique_ptr<perm_storage::PermStore> m_store;
	int64_t m_pluginId = 0;
	int64_t m_lastDegradedLog = 0; // throttles the "no database" messages (unix seconds)
	bool m_listenersRegistered = false;

	// Degraded-start recovery: armed while the store is unusable, one ReloadAsync() at a time.
	bool m_recoveryPending = false;
	bool m_recoveryInFlight = false;
	std::chrono::seconds m_recoveryDelay{kRecoveryInitialDelay};
	std::chrono::steady_clock::time_point m_nextRecoveryAttempt{};
};

PermStoragePlugin g_permStoragePlugin;

} // namespace

// --- listener trampolines ----------------------------------------------------
// No exception may escape into the permissions core (which runs them from API calls made
// by other plugins); an internal failure is logged and the change is accepted, so a bug
// here cannot break the server's permission operations.
namespace {

bool UserRequestListener(int64_t pluginID, uint64_t targetID, const plg::string& username, bool offline) {
	try {
		return g_permStoragePlugin.HandleUserRequest(pluginID, targetID, username, offline);
	} catch (const std::exception& ex) {
		LogError(std::string("perm_storage: exception in OnUserRequest: ") + ex.what());
	} catch (...) {
		LogError("perm_storage: unknown exception in OnUserRequest");
	}
	return true;
}

bool GroupsLoadListener(int64_t pluginID) {
	try {
		return g_permStoragePlugin.HandleGroupsLoad(pluginID);
	} catch (const std::exception& ex) {
		LogError(std::string("perm_storage: exception in OnGroupsLoad: ") + ex.what());
	} catch (...) {
		LogError("perm_storage: unknown exception in OnGroupsLoad");
	}
	return true;
}

void UserDeleteListener(int64_t pluginID, uint64_t targetID) {
	try {
		g_permStoragePlugin.HandleUserDelete(pluginID, targetID);
	} catch (const std::exception& ex) {
		LogError(std::string("perm_storage: exception in OnUserDelete: ") + ex.what());
	} catch (...) {
		LogError("perm_storage: unknown exception in OnUserDelete");
	}
}

bool UserImmunityStorageListener(int64_t pluginID, uint64_t targetID, int32_t immunity) {
	try {
		return g_permStoragePlugin.HandleUserImmunityStorage(pluginID, targetID, immunity);
	} catch (const std::exception& ex) {
		LogError(std::string("perm_storage: exception in OnUserImmunityChangeStorage: ") + ex.what());
	} catch (...) {
		LogError("perm_storage: unknown exception in OnUserImmunityChangeStorage");
	}
	return true;
}

bool UserPermissionStorageListener(int64_t pluginID, permissions::Action action, uint64_t targetID,
                                   const plg::string& perm, permissions::Status oldState,
                                   permissions::Status newState, int64_t oldTimestamp, int64_t newTimestamp) {
	try {
		return g_permStoragePlugin.HandleUserPermissionStorage(pluginID, action, targetID, perm, oldState,
		                                                       newState, oldTimestamp, newTimestamp);
	} catch (const std::exception& ex) {
		LogError(std::string("perm_storage: exception in OnUserPermissionChangeStorage: ") + ex.what());
	} catch (...) {
		LogError("perm_storage: unknown exception in OnUserPermissionChangeStorage");
	}
	return true;
}

bool UserCookieStorageListener(int64_t pluginID, uint64_t targetID, const plg::string& name,
                               const plg::any& cookie) {
	try {
		return g_permStoragePlugin.HandleUserCookieStorage(pluginID, targetID, name, cookie);
	} catch (const std::exception& ex) {
		LogError(std::string("perm_storage: exception in OnUserCookieChangeStorage: ") + ex.what());
	} catch (...) {
		LogError("perm_storage: unknown exception in OnUserCookieChangeStorage");
	}
	return true;
}

bool UserGroupStorageListener(int64_t pluginID, permissions::Action action, uint64_t targetID,
                              const plg::string& group, int64_t oldTimestamp, int64_t newTimestamp) {
	try {
		return g_permStoragePlugin.HandleUserGroupStorage(pluginID, action, targetID, group, oldTimestamp,
		                                                  newTimestamp);
	} catch (const std::exception& ex) {
		LogError(std::string("perm_storage: exception in OnUserGroupChangeStorage: ") + ex.what());
	} catch (...) {
		LogError("perm_storage: unknown exception in OnUserGroupChangeStorage");
	}
	return true;
}

bool GroupCreateStorageListener(int64_t pluginID, const plg::string& name, const plg::vector<plg::string>& perms,
                                int32_t priority, const plg::string& parent) {
	try {
		return g_permStoragePlugin.HandleGroupCreateStorage(pluginID, name, perms, priority, parent);
	} catch (const std::exception& ex) {
		LogError(std::string("perm_storage: exception in OnGroupCreateStorage: ") + ex.what());
	} catch (...) {
		LogError("perm_storage: unknown exception in OnGroupCreateStorage");
	}
	return true;
}

bool GroupDeleteStorageListener(int64_t pluginID, const plg::string& name) {
	try {
		return g_permStoragePlugin.HandleGroupDeleteStorage(pluginID, name);
	} catch (const std::exception& ex) {
		LogError(std::string("perm_storage: exception in OnGroupDeleteStorage: ") + ex.what());
	} catch (...) {
		LogError("perm_storage: unknown exception in OnGroupDeleteStorage");
	}
	return true;
}

bool GroupPermissionStorageListener(int64_t pluginID, permissions::Action action, const plg::string& groupName,
                                    const plg::string& perm, permissions::Status oldState,
                                    permissions::Status newState) {
	try {
		return g_permStoragePlugin.HandleGroupPermissionStorage(pluginID, action, groupName, perm, oldState,
		                                                        newState);
	} catch (const std::exception& ex) {
		LogError(std::string("perm_storage: exception in OnGroupPermissionChangeStorage: ") + ex.what());
	} catch (...) {
		LogError("perm_storage: unknown exception in OnGroupPermissionChangeStorage");
	}
	return true;
}

bool GroupOptionStorageListener(int64_t pluginID, const plg::string& groupName, const plg::string& optionName,
                                const plg::any& value) {
	try {
		return g_permStoragePlugin.HandleGroupOptionStorage(pluginID, groupName, optionName, value);
	} catch (const std::exception& ex) {
		LogError(std::string("perm_storage: exception in OnGroupOptionChangeStorage: ") + ex.what());
	} catch (...) {
		LogError("perm_storage: unknown exception in OnGroupOptionChangeStorage");
	}
	return true;
}

bool SetParentStorageListener(int64_t pluginID, const plg::string& childName, const plg::string& parentName) {
	try {
		return g_permStoragePlugin.HandleSetParentStorage(pluginID, childName, parentName);
	} catch (const std::exception& ex) {
		LogError(std::string("perm_storage: exception in OnSetParentStorage: ") + ex.what());
	} catch (...) {
		LogError("perm_storage: unknown exception in OnSetParentStorage");
	}
	return true;
}

} // namespace

// --- config ------------------------------------------------------------------
// The connection of the plugin comes from perm_storage::LoadDatabaseConfig (src/db_config.hpp):
// databases.cfg first, the legacy keys of config.jsonc - with one warning - only while the section
// does not exist. This function owns the file itself: create it on first start, read it, hand its
// text to the reader.
bool PermStoragePlugin::LoadConfig(std::string& error) {
	if (!plg::GetConfigsDir) {
		error = "plugify config directory is not available";
		return false;
	}

	const std::string configDir = JoinPath(std::string(plg::GetConfigsDir()), "perm_storage");
	const std::string configPath = JoinPath(configDir, "config.jsonc");

	std::error_code ec;
	if (!std::filesystem::exists(configPath, ec)) {
		std::filesystem::create_directories(configDir, ec);
		if (ec) {
			error = "cannot create config directory '" + configDir + "': " + ec.message();
			return false;
		}
		std::ofstream out(configPath, std::ios::binary | std::ios::trunc);
		if (!out) {
			error = "cannot create default config '" + configPath + "'";
			return false;
		}
		out << kDefaultConfig;
		out.close();
		if (!out) {
			error = "failed to write default config '" + configPath + "'";
			return false;
		}
		LogInfo("perm_storage: wrote default config " + configPath);
	}

	std::ifstream in(configPath, std::ios::binary);
	if (!in) {
		error = "cannot open config '" + configPath + "'";
		return false;
	}
	const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

	// A pre-standard config.jsonc keeps supplying the connection, and the reader returns the single
	// migration warning to log for that case; the error stays unprefixed for Degraded().
	const std::string dataDir = plg::GetDataDir ? std::string(plg::GetDataDir()) : std::string();
	perm_storage::DatabaseConfig database;
	std::string warning;
	if (!perm_storage::LoadDatabaseConfig(configPath, text, dataDir, database, warning, error)) {
		return false;
	}
	if (!warning.empty()) {
		LogWarning("perm_storage: " + warning);
	}
	m_dbConfig = std::move(database.db);
	return true;
}

bool PermStoragePlugin::CreateDatabase(std::string& error) {
	m_db = plgdb::Database::Create(m_dbConfig);
	if (!m_db) {
		error = "failed to create the database handle (invalid config)";
		return false;
	}

	m_store = std::make_unique<perm_storage::PermStore>(*m_db);
	m_store->onError = [](std::string_view message) { LogError(message); };
	m_store->onUserLoaded = [](const perm_storage::UserPlan& plan) {
		g_permStoragePlugin.ApplyUserPlan(plan);
	};

	try {
		m_store->EnsureSchema();
	} catch (const std::exception& ex) {
		error = ex.what();
		return false;
	}
	return true;
}

// --- listener registration ---------------------------------------------------
bool PermStoragePlugin::PermissionsBindingsAvailable() {
	return __permissions_CreateGroup && __permissions_GroupExists && __permissions_SetOptionGroup &&
	       __permissions_CreateUser && __permissions_UserExists && __permissions_AddPermission &&
	       __permissions_SetCookie && __permissions_LoadUser && __permissions_OnUserRequest_Register &&
	       __permissions_OnUserDelete_Register && __permissions_OnGroupsLoad_Register &&
	       __permissions_OnUserImmunityChangeStorage_Register &&
	       __permissions_OnUserPermissionChangeStorage_Register &&
	       __permissions_OnUserCookieChangeStorage_Register && __permissions_OnUserGroupChangeStorage_Register &&
	       __permissions_OnGroupCreateStorage_Register && __permissions_OnGroupDeleteStorage_Register &&
	       __permissions_OnGroupPermissionChangeStorage_Register &&
	       __permissions_OnGroupOptionChangeStorage_Register && __permissions_OnSetParentStorage_Register;
}

bool PermStoragePlugin::RegisterListeners(std::string& error) {
	if (!PermissionsBindingsAvailable()) {
		error = "the permissions bindings are not available (is the permissions plugin loaded?)";
		return false;
	}

	// Marked before the first registration so a partial failure unwinds everything.
	m_listenersRegistered = true;

	// Every registration result is checked: a listener that silently did not register would
	// drop the changes it is responsible for.
	struct Registration {
		const char* name;
		permissions::Status status;
	};
	const Registration registrations[] = {
		{"OnUserRequest", permissions::OnUserRequest_Register(&UserRequestListener)},
		{"OnUserDelete", permissions::OnUserDelete_Register(&UserDeleteListener)},
		{"OnGroupsLoad", permissions::OnGroupsLoad_Register(&GroupsLoadListener)},
		{"OnUserImmunityChangeStorage", permissions::OnUserImmunityChangeStorage_Register(&UserImmunityStorageListener)},
		{"OnUserPermissionChangeStorage",
		 permissions::OnUserPermissionChangeStorage_Register(&UserPermissionStorageListener)},
		{"OnUserCookieChangeStorage", permissions::OnUserCookieChangeStorage_Register(&UserCookieStorageListener)},
		{"OnUserGroupChangeStorage", permissions::OnUserGroupChangeStorage_Register(&UserGroupStorageListener)},
		{"OnGroupCreateStorage", permissions::OnGroupCreateStorage_Register(&GroupCreateStorageListener)},
		{"OnGroupDeleteStorage", permissions::OnGroupDeleteStorage_Register(&GroupDeleteStorageListener)},
		{"OnGroupPermissionChangeStorage",
		 permissions::OnGroupPermissionChangeStorage_Register(&GroupPermissionStorageListener)},
		{"OnGroupOptionChangeStorage", permissions::OnGroupOptionChangeStorage_Register(&GroupOptionStorageListener)},
		{"OnSetParentStorage", permissions::OnSetParentStorage_Register(&SetParentStorageListener)},
	};

	for (const Registration& registration : registrations) {
		if (registration.status != permissions::Status::Success) {
			UnregisterListeners();
			error = std::string("registering the ") + registration.name + " listener failed (status " +
			        StatusText(registration.status) + ")";
			return false;
		}
	}
	return true;
}

void PermStoragePlugin::UnregisterListeners() {
	if (!m_listenersRegistered) {
		return;
	}
	m_listenersRegistered = false;
	if (__permissions_OnUserRequest_Unregister) permissions::OnUserRequest_Unregister(&UserRequestListener);
	if (__permissions_OnUserDelete_Unregister) permissions::OnUserDelete_Unregister(&UserDeleteListener);
	if (__permissions_OnGroupsLoad_Unregister) permissions::OnGroupsLoad_Unregister(&GroupsLoadListener);
	if (__permissions_OnUserImmunityChangeStorage_Unregister)
		permissions::OnUserImmunityChangeStorage_Unregister(&UserImmunityStorageListener);
	if (__permissions_OnUserPermissionChangeStorage_Unregister)
		permissions::OnUserPermissionChangeStorage_Unregister(&UserPermissionStorageListener);
	if (__permissions_OnUserCookieChangeStorage_Unregister)
		permissions::OnUserCookieChangeStorage_Unregister(&UserCookieStorageListener);
	if (__permissions_OnUserGroupChangeStorage_Unregister)
		permissions::OnUserGroupChangeStorage_Unregister(&UserGroupStorageListener);
	if (__permissions_OnGroupCreateStorage_Unregister)
		permissions::OnGroupCreateStorage_Unregister(&GroupCreateStorageListener);
	if (__permissions_OnGroupDeleteStorage_Unregister)
		permissions::OnGroupDeleteStorage_Unregister(&GroupDeleteStorageListener);
	if (__permissions_OnGroupPermissionChangeStorage_Unregister)
		permissions::OnGroupPermissionChangeStorage_Unregister(&GroupPermissionStorageListener);
	if (__permissions_OnGroupOptionChangeStorage_Unregister)
		permissions::OnGroupOptionChangeStorage_Unregister(&GroupOptionStorageListener);
	if (__permissions_OnSetParentStorage_Unregister)
		permissions::OnSetParentStorage_Unregister(&SetParentStorageListener);
}

// --- core application --------------------------------------------------------
void PermStoragePlugin::ApplyGroupsToCore() {
	if (!m_store) {
		return;
	}
	for (const perm_storage::GroupPlan& group : m_store->Groups()) {
		if (permissions::GroupExists(plg::string(group.name))) {
			continue; // already in the core: never touch a group a plugin may have changed
		}
		plg::vector<plg::string> perms;
		perms.reserve(group.permissions.size());
		for (const std::string& line : group.permissions) {
			perms.push_back(plg::string(line));
		}
		// The snapshot is parent-first, so an existing parent has been created already; a
		// parent that is missing from the core (deleted group, cycle, incomplete database
		// row) is dropped instead of failing the whole group.
		const bool hasParent = !group.parent.empty() && permissions::GroupExists(plg::string(group.parent));
		if (!group.parent.empty() && !hasParent) {
			LogWarning("perm_storage: group '" + group.name + "' references unknown parent '" + group.parent +
			           "', creating it as a root");
		}
		permissions::Status status =
			permissions::CreateGroup(m_pluginId, plg::string(group.name), perms,
			                         static_cast<int32_t>(group.priority),
			                         hasParent ? plg::string(group.parent) : plg::string(), /*dontBroadcast=*/true);
		if (status != permissions::Status::Success && hasParent) {
			// e.g. a parent cycle stored in the database: retry as a root so the group and
			// its permissions still reach the core.
			LogWarning("perm_storage: creating group '" + group.name + "' under '" + group.parent +
			           "' failed (status " + StatusText(status) + "), retrying as a root");
			status = permissions::CreateGroup(m_pluginId, plg::string(group.name), perms,
			                                  static_cast<int32_t>(group.priority), plg::string(),
			                                  /*dontBroadcast=*/true);
		}
		if (status != permissions::Status::Success) {
			LogError("perm_storage: creating group '" + group.name + "' failed (status " +
			         StatusText(status) + ")");
			continue;
		}
		for (const perm_storage::UserCookie& option : group.options) {
			permissions::SetOptionGroup(m_pluginId, plg::string(group.name), plg::string(option.name),
			                            ValueToAny(option.value), /*dontBroadcast=*/true);
		}
	}
}

void PermStoragePlugin::ApplyUserPlan(const perm_storage::UserPlan& plan) {
	const std::string steam = std::to_string(plan.steamid);

	// Groups ("name" or "name timestamp"); CreateUser() rejects the whole list when one
	// group is unknown, so memberships of deleted groups are dropped here.
	plg::vector<plg::string> groupList;
	groupList.reserve(plan.groups.size());
	for (const perm_storage::UserGroupMembership& membership : plan.groups) {
		if (!permissions::GroupExists(plg::string(membership.group))) {
			LogWarning("perm_storage: dropping unknown group '" + membership.group + "' of user " + steam);
			continue;
		}
		std::string entry = membership.group;
		if (membership.expiresAt != 0) {
			entry.push_back(' ');
			entry.append(std::to_string(membership.expiresAt));
		}
		groupList.push_back(plg::string(entry));
	}

	if (permissions::UserExists(plan.steamid) == permissions::PlayerState::NotFound) {
		const permissions::Status status = permissions::CreateUser(
			m_pluginId, plan.steamid, static_cast<int32_t>(plan.immunity), plan.offline, groupList);
		if (status == permissions::Status::UserAlreadyExist) {
			LogWarning("perm_storage: user " + steam + " was created concurrently, applying the stored data");
		} else if (status != permissions::Status::Success) {
			LogError("perm_storage: CreateUser failed for " + steam + " (status " + StatusText(status) + ")");
			if (m_store) {
				m_store->ForgetUser(plan.steamid); // let a later request try again
			}
			return; // no LoadUser broadcast for a user that is not in the core
		}
	} else {
		LogWarning("perm_storage: user " + steam + " already exists in the core, applying the stored data");
	}

	for (const perm_storage::UserPermission& permission : plan.permissions) {
		const std::string line = (permission.denied ? "-" : "") + permission.perm;
		const permissions::Status status = permissions::AddPermission(m_pluginId, plan.steamid, plg::string(line),
		                                                              permission.expiresAt,
		                                                              /*dontBroadcast=*/true);
		if (status != permissions::Status::Success && status != permissions::Status::PermAlreadyGranted &&
		    status != permissions::Status::PermConflict) {
			LogError("perm_storage: replaying permission '" + line + "' for " + steam + " failed (status " +
			         StatusText(status) + ")");
		}
	}

	for (const perm_storage::UserCookie& cookie : plan.cookies) {
		const permissions::Status status = permissions::SetCookie(m_pluginId, plan.steamid,
		                                                          plg::string(cookie.name),
		                                                          ValueToAny(cookie.value),
		                                                          /*dontBroadcast=*/true);
		if (status != permissions::Status::Success) {
			LogError("perm_storage: replaying cookie '" + cookie.name + "' for " + steam + " failed (status " +
			         StatusText(status) + ")");
		}
	}

	// Announces the end of the load: re-dispatches OnUserRequest (answered with "loaded"
	// now) and then broadcasts OnUserLoaded to the connector.
	const permissions::Status status =
		permissions::LoadUser(m_pluginId, plan.steamid, plg::string(plan.username), plan.offline,
		                      /*dontBroadcast=*/false);
	if (status != permissions::Status::Success) {
		LogError("perm_storage: LoadUser broadcast for " + steam + " failed (status " + StatusText(status) +
		         "); another storage plugin rejected the request");
	}
}

// --- listener bodies ---------------------------------------------------------
bool PermStoragePlugin::HandleUserRequest(int64_t pluginID, uint64_t targetID, const plg::string& username,
                                          bool offline) {
	if (pluginID == m_pluginId) {
		// Our own completion broadcast: the user is loaded, let the caller proceed.
		return true;
	}
	if (!m_store || !m_store->GroupsLoaded()) {
		// Degraded (no database, or the group snapshot could not be read yet): answer DBNotReady
		// like a storage whose DB is unavailable, so the connector keeps retrying and logs that
		// no storage is answering. Applying a plan before the groups are in the core would drop
		// every membership of the user, and a silent "loaded" broadcast for a user that was
		// never created would hide the outage.
		const int64_t now = static_cast<int64_t>(std::time(nullptr));
		if (now - m_lastDegradedLog >= 60) {
			m_lastDegradedLog = now;
			LogError("perm_storage: no usable database, rejecting the load request of user " +
			         std::to_string(targetID) + " (DBNotReady)");
		}
		return false;
	}
	// Started/AlreadyLoading -> false -> LoadUser() returns DBNotReady, and this plugin
	// re-issues LoadUser(pluginId, .., dontBroadcast=false) once the plan has been applied.
	return m_store->RequestUserLoad(targetID, ToStd(username), offline) ==
	       perm_storage::PermStore::LoadRequest::AlreadyLoaded;
}

bool PermStoragePlugin::HandleGroupsLoad(int64_t pluginID) {
	(void)pluginID;
	if (!m_store || !m_store->GroupsLoaded()) {
		// No database or no snapshot read yet: veto the load (DBNotReady) instead of letting
		// the requester accept a silently empty group set; a later load succeeds once the
		// recovery pump has reread the groups.
		return false;
	}
	ApplyGroupsToCore(); // cheap no-op unless a group is missing in the core
	return true;
}

void PermStoragePlugin::HandleUserDelete(int64_t pluginID, uint64_t targetID) {
	(void)pluginID;
	if (m_store) {
		// The user left (or an admin deleted it): the persisted data stays, but the next
		// load request must read it again.
		m_store->ForgetUser(targetID);
	}
}

bool PermStoragePlugin::HandleUserImmunityStorage(int64_t pluginID, uint64_t targetID, int32_t immunity) {
	if (!m_store || pluginID == m_pluginId) {
		return true;
	}
	m_store->SaveUserImmunity(targetID, immunity);
	return true;
}

bool PermStoragePlugin::HandleUserPermissionStorage(int64_t pluginID, permissions::Action action,
                                                    uint64_t targetID, const plg::string& perm,
                                                    permissions::Status /*oldState*/,
                                                    permissions::Status newState, int64_t /*oldTimestamp*/,
                                                    int64_t newTimestamp) {
	if (!m_store || pluginID == m_pluginId) {
		return true;
	}
	m_store->SaveUserPermission(targetID, ToPermChange(action, ToStd(perm), newState, newTimestamp));
	return true;
}

bool PermStoragePlugin::HandleUserCookieStorage(int64_t pluginID, uint64_t targetID, const plg::string& name,
                                                const plg::any& cookie) {
	if (!m_store || pluginID == m_pluginId) {
		return true;
	}
	const std::optional<perm_storage::Value> value = AnyToValue(cookie);
	if (!value) {
		// Rejected: the change is cancelled (the caller sees DBNotReady) because the value
		// could not be persisted.
		LogError("perm_storage: rejecting user cookie '" + ToStd(name) + "' of " +
		         std::to_string(targetID) + ": unsupported value type");
		return false;
	}
	m_store->SaveUserCookie(targetID, std::string_view(name), *value);
	return true;
}

bool PermStoragePlugin::HandleUserGroupStorage(int64_t pluginID, permissions::Action action, uint64_t targetID,
                                               const plg::string& group, int64_t /*oldTimestamp*/,
                                               int64_t newTimestamp) {
	if (!m_store || pluginID == m_pluginId) {
		return true;
	}
	perm_storage::GroupChange change;
	change.group = ToStd(group);
	change.expiresAt = newTimestamp;
	switch (action) {
	case permissions::Action::Remove: change.action = perm_storage::GroupAction::Remove; break;
	case permissions::Action::Replace: change.action = perm_storage::GroupAction::Replace; break;
	case permissions::Action::Add:
	case permissions::Action::ReplaceToWC: // not produced for user groups; treated as Add
		change.action = perm_storage::GroupAction::Add;
		break;
	}
	m_store->SaveUserGroup(targetID, change);
	return true;
}

bool PermStoragePlugin::HandleGroupCreateStorage(int64_t pluginID, const plg::string& name,
                                                 const plg::vector<plg::string>& perms, int32_t priority,
                                                 const plg::string& parent) {
	if (!m_store || pluginID == m_pluginId) {
		return true;
	}
	std::vector<std::string> permissions;
	permissions.reserve(perms.size());
	for (const plg::string& perm : perms) {
		permissions.emplace_back(ToView(perm));
	}
	m_store->SaveGroup(ToStd(name), priority, ToStd(parent), std::move(permissions));
	return true;
}

bool PermStoragePlugin::HandleGroupDeleteStorage(int64_t pluginID, const plg::string& name) {
	if (!m_store || pluginID == m_pluginId) {
		return true;
	}
	m_store->DropGroup(std::string_view(name));
	return true;
}

bool PermStoragePlugin::HandleGroupPermissionStorage(int64_t pluginID, permissions::Action action,
                                                     const plg::string& groupName, const plg::string& perm,
                                                     permissions::Status /*oldState*/,
                                                     permissions::Status newState) {
	if (!m_store || pluginID == m_pluginId) {
		return true;
	}
	m_store->SaveGroupPermission(std::string_view(groupName),
	                             ToPermChange(action, ToStd(perm), newState, /*newTimestamp=*/0));
	return true;
}

bool PermStoragePlugin::HandleGroupOptionStorage(int64_t pluginID, const plg::string& groupName,
                                                 const plg::string& optionName, const plg::any& value) {
	if (!m_store || pluginID == m_pluginId) {
		return true;
	}
	const std::optional<perm_storage::Value> stored = AnyToValue(value);
	if (!stored) {
		LogError("perm_storage: rejecting option '" + ToStd(optionName) + "' of group '" +
		         ToStd(groupName) + "': unsupported value type");
		return false;
	}
	m_store->SaveGroupOption(std::string_view(groupName), std::string_view(optionName), *stored);
	return true;
}

bool PermStoragePlugin::HandleSetParentStorage(int64_t pluginID, const plg::string& childName,
                                               const plg::string& parentName) {
	if (!m_store || pluginID == m_pluginId) {
		return true;
	}
	m_store->SaveGroupParent(std::string_view(childName), std::string_view(parentName));
	return true;
}

// --- lifecycle ---------------------------------------------------------------
plg::PluginResult PermStoragePlugin::Degraded(std::string_view what, std::string_view error) {
	LogError("perm_storage: " + std::string(what) + ": " + std::string(error) +
	         " - running degraded: permission changes are not persisted");
	return {}; // keep the server running; the core works in-memory without a storage plugin
}

void PermStoragePlugin::ArmDatabaseRecovery() {
	if (m_recoveryPending || !m_db || !m_store) {
		// Already armed, or nothing to retry with: Database::Create() only fails for a config
		// that no retry can fix, so those plugins stay degraded as before.
		return;
	}
	m_recoveryPending = true;
	m_recoveryDelay = kRecoveryInitialDelay;
	m_nextRecoveryAttempt = std::chrono::steady_clock::now() + m_recoveryDelay;
}

void PermStoragePlugin::RunDatabaseRecovery() {
	if (!m_recoveryPending || m_recoveryInFlight) {
		return;
	}
	const auto now = std::chrono::steady_clock::now();
	if (now < m_nextRecoveryAttempt) {
		return;
	}

	// Runs on the worker thread; the callback lands in Poll(), so the game thread only waits
	// for the backoff between attempts, never for the database.
	m_recoveryInFlight = true;
	m_store->ReloadAsync([this](bool ok) {
		m_recoveryInFlight = false;
		if (!ok) {
			// Still down: no log per attempt, only a longer wait (the degradation itself was
			// reported once when it was detected).
			m_recoveryDelay = std::min(m_recoveryDelay * 2, kRecoveryMaxDelay);
			m_nextRecoveryAttempt = std::chrono::steady_clock::now() + m_recoveryDelay;
			return;
		}
		m_recoveryPending = false;
		m_recoveryDelay = kRecoveryInitialDelay;
		LogInfo("perm_storage: database is back: schema and groups loaded, the degraded start is over");
		ApplyGroupsToCore();
		if (!m_listenersRegistered) {
			std::string error;
			if (!RegisterListeners(error)) {
				LogError("perm_storage: " + error);
			}
		}
	});
}

plg::PluginResult PermStoragePlugin::OnPluginStart() {
	try {
		if (!plg::plugin::GetId) {
			return Degraded("startup", "the plugin id is not available");
		}
		m_pluginId = static_cast<int64_t>(GetId());

		std::string error;
		// Before anything else: without the core's methods there is nothing this plugin could
		// do, and calling a null slot would take the server down.
		if (!PermissionsBindingsAvailable()) {
			return Degraded("startup", "the permissions bindings are not available (is the permissions plugin loaded?)");
		}
		if (!LoadConfig(error)) {
			return Degraded("startup", error);
		}
		if (!CreateDatabase(error)) {
			// The database can be down while the server boots (or the schema cannot be created
			// yet): keep the handle and let the retry pump finish the job later.
			ArmDatabaseRecovery();
			return Degraded("startup", error);
		}

		// Startup only: pull the groups in synchronously and feed them to the core before
		// the storage listeners go live, so this replay cannot write anything back.
		const std::vector<perm_storage::GroupPlan>& groups = m_store->LoadGroupsSync();
		ApplyGroupsToCore();
		if (m_store->GroupsLoaded()) {
			LogInfo("perm_storage: loaded " + std::to_string(groups.size()) + " group(s) from the database");
		} else {
			// User data can still be persisted (the tables exist), so the plugin keeps running
			// with an empty snapshot until the recovery pump has reread the groups.
			LogError("perm_storage: reading the groups failed, retrying in the background");
			ArmDatabaseRecovery();
		}

		if (!RegisterListeners(error)) {
			return Degraded("startup", error);
		}

		LogInfo("perm_storage: started (driver: " +
		        std::string(m_dbConfig.driver == plgdb::Driver::MySQL ? "mysql" : "sqlite") +
		        ", plugin id " + std::to_string(m_pluginId) + ")");
		return {};
	} catch (const std::exception& ex) {
		return Degraded("startup", ex.what());
	} catch (...) {
		return Degraded("startup", "unknown exception");
	}
}

plg::PluginResult PermStoragePlugin::OnPluginUpdate(std::chrono::milliseconds) {
	if (m_db) {
		try {
			m_db->Poll();
		} catch (const std::exception& ex) {
			LogError(std::string("perm_storage: database poll failed: ") + ex.what());
		} catch (...) {
			LogError("perm_storage: database poll failed with an unknown exception");
		}
	}
	if (m_store) {
		try {
			m_store->RunRetries();
		} catch (const std::exception& ex) {
			LogError(std::string("perm_storage: retry pump failed: ") + ex.what());
		} catch (...) {
			LogError("perm_storage: retry pump failed with an unknown exception");
		}
	}
	// After Poll(): a recovery attempt that completed has been applied by now.
	try {
		RunDatabaseRecovery();
	} catch (const std::exception& ex) {
		LogError(std::string("perm_storage: database recovery failed: ") + ex.what());
	} catch (...) {
		LogError("perm_storage: database recovery failed with an unknown exception");
	}
	return {};
}

plg::PluginResult PermStoragePlugin::OnPluginEnd() {
	try {
		UnregisterListeners();
		m_store.reset();
		m_db.reset(); // flushes the pending writes on the worker and joins it
	} catch (const std::exception& ex) {
		LogError(std::string("perm_storage: OnPluginEnd failed: ") + ex.what());
	} catch (...) {
		LogError("perm_storage: OnPluginEnd failed with an unknown exception");
	}
	return {};
}

PLUGIFY_PLUGIN(PLUGIN_API, &g_permStoragePlugin)
