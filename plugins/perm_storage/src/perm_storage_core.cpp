#include "perm_storage_core.hpp"

#include <algorithm>
#include <charconv>
#include <ctime>
#include <stdexcept>
#include <utility>

namespace perm_storage {
namespace {

using plgdb::Driver;
using plgdb::Params;
using plgdb::Result;
using plgdb::Statement;

int64_t Now() {
	return static_cast<int64_t>(std::time(nullptr));
}

// Permission lines are stored without the '-' deny prefix; the state lives in its own
// column (1 = Allow, 0 = Disallow), mirroring what the core keeps in memory.
bool IsDeniedLine(std::string_view line) {
	return !line.empty() && line.front() == '-';
}

std::string_view StripDenyPrefix(std::string_view line) {
	return IsDeniedLine(line) ? line.substr(1) : line;
}

// "a.*" -> "a" (the plain permission a ReplaceToWC change replaced). Only meaningful for
// wildcard lines; for anything else the input is returned unchanged.
std::string_view WildcardRootOf(std::string_view perm) {
	return perm.size() > 2 && perm.ends_with(".*") ? perm.substr(0, perm.size() - 2) : perm;
}

// --- schema ------------------------------------------------------------------
struct DdlStatement {
	std::string_view mysql;
	std::string_view sqlite;
};

constexpr DdlStatement kCreateTables[] = {
	{DdlStatement{
	     "CREATE TABLE IF NOT EXISTS perm_users ("
	     "steamid64 BIGINT NOT NULL, "
	     "name VARCHAR(255) NOT NULL DEFAULT '', "
	     "immunity INT NOT NULL DEFAULT -1, "
	     "created_at BIGINT NOT NULL DEFAULT 0, "
	     "updated_at BIGINT NOT NULL DEFAULT 0, "
	     "PRIMARY KEY (steamid64)"
	     ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4",
	     "CREATE TABLE IF NOT EXISTS perm_users ("
	     "steamid64 INTEGER NOT NULL, "
	     "name TEXT NOT NULL DEFAULT '', "
	     "immunity INTEGER NOT NULL DEFAULT -1, "
	     "created_at INTEGER NOT NULL DEFAULT 0, "
	     "updated_at INTEGER NOT NULL DEFAULT 0, "
	     "PRIMARY KEY (steamid64)"
	     ")"}},
	{DdlStatement{
	     "CREATE TABLE IF NOT EXISTS perm_user_permissions ("
	     "steamid64 BIGINT NOT NULL, "
	     "perm VARCHAR(255) NOT NULL, "
	     "state TINYINT NOT NULL DEFAULT 1, "
	     "expires_at BIGINT NOT NULL DEFAULT 0, "
	     "PRIMARY KEY (steamid64, perm)"
	     ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4",
	     "CREATE TABLE IF NOT EXISTS perm_user_permissions ("
	     "steamid64 INTEGER NOT NULL, "
	     "perm TEXT NOT NULL, "
	     "state INTEGER NOT NULL DEFAULT 1, "
	     "expires_at INTEGER NOT NULL DEFAULT 0, "
	     "PRIMARY KEY (steamid64, perm)"
	     ")"}},
	{DdlStatement{
	     "CREATE TABLE IF NOT EXISTS perm_user_groups ("
	     "steamid64 BIGINT NOT NULL, "
	     "group_name VARCHAR(255) NOT NULL, "
	     "expires_at BIGINT NOT NULL DEFAULT 0, "
	     "PRIMARY KEY (steamid64, group_name)"
	     ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4",
	     "CREATE TABLE IF NOT EXISTS perm_user_groups ("
	     "steamid64 INTEGER NOT NULL, "
	     "group_name TEXT NOT NULL, "
	     "expires_at INTEGER NOT NULL DEFAULT 0, "
	     "PRIMARY KEY (steamid64, group_name)"
	     ")"}},
	{DdlStatement{
	     "CREATE TABLE IF NOT EXISTS perm_user_cookies ("
	     "steamid64 BIGINT NOT NULL, "
	     "name VARCHAR(255) NOT NULL, "
	     "value_type CHAR(1) NOT NULL, "
	     "cookie_value TEXT NOT NULL, "
	     "PRIMARY KEY (steamid64, name)"
	     ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4",
	     "CREATE TABLE IF NOT EXISTS perm_user_cookies ("
	     "steamid64 INTEGER NOT NULL, "
	     "name TEXT NOT NULL, "
	     "value_type TEXT NOT NULL, "
	     "cookie_value TEXT NOT NULL, "
	     "PRIMARY KEY (steamid64, name)"
	     ")"}},
	{DdlStatement{
	     "CREATE TABLE IF NOT EXISTS perm_groups ("
	     "name VARCHAR(255) NOT NULL, "
	     "priority INT NOT NULL DEFAULT 0, "
	     "parent VARCHAR(255) NULL, "
	     "PRIMARY KEY (name)"
	     ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4",
	     "CREATE TABLE IF NOT EXISTS perm_groups ("
	     "name TEXT NOT NULL, "
	     "priority INTEGER NOT NULL DEFAULT 0, "
	     "parent TEXT NULL, "
	     "PRIMARY KEY (name)"
	     ")"}},
	{DdlStatement{
	     "CREATE TABLE IF NOT EXISTS perm_group_permissions ("
	     "group_name VARCHAR(255) NOT NULL, "
	     "perm VARCHAR(255) NOT NULL, "
	     "state TINYINT NOT NULL DEFAULT 1, "
	     "PRIMARY KEY (group_name, perm)"
	     ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4",
	     "CREATE TABLE IF NOT EXISTS perm_group_permissions ("
	     "group_name TEXT NOT NULL, "
	     "perm TEXT NOT NULL, "
	     "state INTEGER NOT NULL DEFAULT 1, "
	     "PRIMARY KEY (group_name, perm)"
	     ")"}},
	{DdlStatement{
	     "CREATE TABLE IF NOT EXISTS perm_group_options ("
	     "group_name VARCHAR(255) NOT NULL, "
	     "option_name VARCHAR(255) NOT NULL, "
	     "value_type CHAR(1) NOT NULL, "
	     "option_value TEXT NOT NULL, "
	     "PRIMARY KEY (group_name, option_name)"
	     ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4",
	     "CREATE TABLE IF NOT EXISTS perm_group_options ("
	     "group_name TEXT NOT NULL, "
	     "option_name TEXT NOT NULL, "
	     "value_type TEXT NOT NULL, "
	     "option_value TEXT NOT NULL, "
	     "PRIMARY KEY (group_name, option_name)"
	     ")"}},
};

constexpr size_t kCreateTableCount = sizeof(kCreateTables) / sizeof(kCreateTables[0]);

// --- statements (driver-specific upserts) ------------------------------------
constexpr std::string_view kSelectGroups =
	"SELECT name, priority, parent FROM perm_groups ORDER BY name";
constexpr std::string_view kSelectGroupPermissions =
	"SELECT group_name, perm, state FROM perm_group_permissions ORDER BY group_name, perm";
constexpr std::string_view kSelectGroupOptions =
	"SELECT group_name, option_name, value_type, option_value FROM perm_group_options "
	"ORDER BY group_name, option_name";

constexpr std::string_view kSelectUser = "SELECT name, immunity FROM perm_users WHERE steamid64 = ?";
constexpr std::string_view kSelectUserPermissions =
	"SELECT perm, state, expires_at FROM perm_user_permissions WHERE steamid64 = ? ORDER BY perm";
constexpr std::string_view kSelectUserGroups =
	"SELECT group_name, expires_at FROM perm_user_groups WHERE steamid64 = ? ORDER BY group_name";
constexpr std::string_view kSelectUserCookies =
	"SELECT name, value_type, cookie_value FROM perm_user_cookies WHERE steamid64 = ? ORDER BY name";

constexpr std::string_view kUpsertUserSeenMySQL =
	"INSERT INTO perm_users (steamid64, name, immunity, created_at, updated_at) VALUES (?, ?, -1, ?, ?) "
	"ON DUPLICATE KEY UPDATE name = VALUES(name), updated_at = VALUES(updated_at)";
constexpr std::string_view kUpsertUserSeenSQLite =
	"INSERT INTO perm_users (steamid64, name, immunity, created_at, updated_at) VALUES (?, ?, -1, ?, ?) "
	"ON CONFLICT(steamid64) DO UPDATE SET name = excluded.name, updated_at = excluded.updated_at";

constexpr std::string_view kUpsertUserImmunityMySQL =
	"INSERT INTO perm_users (steamid64, immunity, created_at, updated_at) VALUES (?, ?, ?, ?) "
	"ON DUPLICATE KEY UPDATE immunity = VALUES(immunity)";
constexpr std::string_view kUpsertUserImmunitySQLite =
	"INSERT INTO perm_users (steamid64, immunity, created_at, updated_at) VALUES (?, ?, ?, ?) "
	"ON CONFLICT(steamid64) DO UPDATE SET immunity = excluded.immunity";

constexpr std::string_view kUpsertUserPermissionMySQL =
	"INSERT INTO perm_user_permissions (steamid64, perm, state, expires_at) VALUES (?, ?, ?, ?) "
	"ON DUPLICATE KEY UPDATE state = VALUES(state), expires_at = VALUES(expires_at)";
constexpr std::string_view kUpsertUserPermissionSQLite =
	"INSERT INTO perm_user_permissions (steamid64, perm, state, expires_at) VALUES (?, ?, ?, ?) "
	"ON CONFLICT(steamid64, perm) DO UPDATE SET state = excluded.state, expires_at = excluded.expires_at";
constexpr std::string_view kDeleteUserPermission =
	"DELETE FROM perm_user_permissions WHERE steamid64 = ? AND perm = ?";

constexpr std::string_view kUpsertUserGroupMySQL =
	"INSERT INTO perm_user_groups (steamid64, group_name, expires_at) VALUES (?, ?, ?) "
	"ON DUPLICATE KEY UPDATE expires_at = VALUES(expires_at)";
constexpr std::string_view kUpsertUserGroupSQLite =
	"INSERT INTO perm_user_groups (steamid64, group_name, expires_at) VALUES (?, ?, ?) "
	"ON CONFLICT(steamid64, group_name) DO UPDATE SET expires_at = excluded.expires_at";
constexpr std::string_view kDeleteUserGroup =
	"DELETE FROM perm_user_groups WHERE steamid64 = ? AND group_name = ?";

constexpr std::string_view kUpsertUserCookieMySQL =
	"INSERT INTO perm_user_cookies (steamid64, name, value_type, cookie_value) VALUES (?, ?, ?, ?) "
	"ON DUPLICATE KEY UPDATE value_type = VALUES(value_type), cookie_value = VALUES(cookie_value)";
constexpr std::string_view kUpsertUserCookieSQLite =
	"INSERT INTO perm_user_cookies (steamid64, name, value_type, cookie_value) VALUES (?, ?, ?, ?) "
	"ON CONFLICT(steamid64, name) DO UPDATE SET value_type = excluded.value_type, cookie_value = excluded.cookie_value";

constexpr std::string_view kUpsertGroupMySQL =
	"INSERT INTO perm_groups (name, priority, parent) VALUES (?, ?, ?) "
	"ON DUPLICATE KEY UPDATE priority = VALUES(priority), parent = VALUES(parent)";
constexpr std::string_view kUpsertGroupSQLite =
	"INSERT INTO perm_groups (name, priority, parent) VALUES (?, ?, ?) "
	"ON CONFLICT(name) DO UPDATE SET priority = excluded.priority, parent = excluded.parent";
constexpr std::string_view kDeleteGroup = "DELETE FROM perm_groups WHERE name = ?";
constexpr std::string_view kClearGroupParent = "UPDATE perm_groups SET parent = NULL WHERE parent = ?";
constexpr std::string_view kSetGroupParent = "UPDATE perm_groups SET parent = ? WHERE name = ?";
constexpr std::string_view kDeleteGroupPermissions = "DELETE FROM perm_group_permissions WHERE group_name = ?";
constexpr std::string_view kDeleteGroupOptions = "DELETE FROM perm_group_options WHERE group_name = ?";
constexpr std::string_view kDeleteGroupMemberships = "DELETE FROM perm_user_groups WHERE group_name = ?";

constexpr std::string_view kUpsertGroupPermissionMySQL =
	"INSERT INTO perm_group_permissions (group_name, perm, state) VALUES (?, ?, ?) "
	"ON DUPLICATE KEY UPDATE state = VALUES(state)";
constexpr std::string_view kUpsertGroupPermissionSQLite =
	"INSERT INTO perm_group_permissions (group_name, perm, state) VALUES (?, ?, ?) "
	"ON CONFLICT(group_name, perm) DO UPDATE SET state = excluded.state";
constexpr std::string_view kDeleteGroupPermission =
	"DELETE FROM perm_group_permissions WHERE group_name = ? AND perm = ?";

constexpr std::string_view kUpsertGroupOptionMySQL =
	"INSERT INTO perm_group_options (group_name, option_name, value_type, option_value) VALUES (?, ?, ?, ?) "
	"ON DUPLICATE KEY UPDATE value_type = VALUES(value_type), option_value = VALUES(option_value)";
constexpr std::string_view kUpsertGroupOptionSQLite =
	"INSERT INTO perm_group_options (group_name, option_name, value_type, option_value) VALUES (?, ?, ?, ?) "
	"ON CONFLICT(group_name, option_name) DO UPDATE SET value_type = excluded.value_type, option_value = excluded.option_value";

std::string_view Pick(Driver driver, std::string_view mysql, std::string_view sqlite) {
	return driver == Driver::MySQL ? mysql : sqlite;
}

} // namespace

// --- Value -------------------------------------------------------------------
Value Value::MakeBool(bool v) {
	Value value;
	value.kind = Kind::Bool;
	value.boolean = v;
	return value;
}

Value Value::MakeInt(int64_t v) {
	Value value;
	value.kind = Kind::Int;
	value.integer = v;
	return value;
}

Value Value::MakeDouble(double v) {
	Value value;
	value.kind = Kind::Double;
	value.real = v;
	return value;
}

Value Value::MakeString(std::string v) {
	Value value;
	value.kind = Kind::String;
	value.text = std::move(v);
	return value;
}

std::string Value::Encode() const {
	switch (kind) {
	case Kind::Bool: return boolean ? "1" : "0";
	case Kind::Int: return std::to_string(integer);
	case Kind::Double: {
		// Shortest representation that parses back to the exact same double.
		char buffer[64];
		const std::to_chars_result result = std::to_chars(buffer, buffer + sizeof(buffer), real);
		if (result.ec != std::errc{}) {
			return std::to_string(real); // unreachable for finite doubles, but never lose the row
		}
		return std::string(buffer, result.ptr);
	}
	case Kind::String: return text;
	}
	return {};
}

std::optional<Value> Value::Decode(char type, std::string_view encoded) {
	switch (static_cast<Kind>(type)) {
	case Kind::Bool:
		if (encoded == "1") return MakeBool(true);
		if (encoded == "0") return MakeBool(false);
		return std::nullopt;
	case Kind::Int: {
		int64_t parsed = 0;
		const auto result = std::from_chars(encoded.data(), encoded.data() + encoded.size(), parsed);
		if (result.ec != std::errc{} || result.ptr != encoded.data() + encoded.size()) return std::nullopt;
		return MakeInt(parsed);
	}
	case Kind::Double: {
		double parsed = 0.0;
		const auto result = std::from_chars(encoded.data(), encoded.data() + encoded.size(), parsed);
		if (result.ec != std::errc{} || result.ptr != encoded.data() + encoded.size()) return std::nullopt;
		return MakeDouble(parsed);
	}
	case Kind::String: return MakeString(std::string(encoded));
	}
	return std::nullopt;
}

// --- group ordering ----------------------------------------------------------
std::vector<GroupPlan> SortGroupsByParent(std::vector<GroupPlan> groups,
                                          const std::function<void(std::string_view)>& onError) {
	const auto report = [&onError](std::string_view message) {
		if (onError) {
			onError(message);
		}
	};

	const size_t count = groups.size();
	std::map<std::string_view, size_t> byName;
	for (size_t i = 0; i < count; ++i) {
		byName[groups[i].name] = i; // duplicates: last one wins, the extra row is still emitted
	}

	std::vector<bool> hasParent(count, false);
	std::vector<std::vector<size_t>> children(count);
	for (size_t i = 0; i < count; ++i) {
		const std::string& parent = groups[i].parent;
		if (parent.empty()) {
			continue;
		}
		const auto it = byName.find(parent);
		if (it == byName.end()) {
			// The plan keeps the parent (it is data); creating the group without it is up to
			// the caller, which knows what exists in the core.
			report("perm_storage: group '" + groups[i].name + "' has unknown parent '" + parent +
			       "', treating it as a root");
			continue;
		}
		if (it->second == i) {
			report("perm_storage: group '" + groups[i].name + "' is its own parent, treating it as a root");
			continue;
		}
		hasParent[i] = true;
		children[it->second].push_back(i);
	}

	std::vector<GroupPlan> out;
	out.reserve(count);
	std::vector<bool> emitted(count, false);
	std::vector<size_t> stack;

	// Parents are emitted before their children (pre-order DFS, roots in input order).
	// Every group has at most one parent, so a cycle is never reachable from a root.
	for (size_t root = 0; root < count; ++root) {
		if (hasParent[root] || emitted[root]) {
			continue;
		}
		stack.push_back(root);
		while (!stack.empty()) {
			const size_t index = stack.back();
			stack.pop_back();
			if (emitted[index]) {
				continue;
			}
			emitted[index] = true;
			out.push_back(std::move(groups[index]));
			const std::vector<size_t>& kids = children[index];
			for (auto it = kids.rbegin(); it != kids.rend(); ++it) {
				if (!emitted[*it]) {
					stack.push_back(*it);
				}
			}
		}
	}

	// Leftovers belong to a parent cycle (their whole subtree is unreachable): emit them in
	// input order so the caller can still try to create them.
	for (size_t i = 0; i < count; ++i) {
		if (emitted[i]) {
			continue;
		}
		emitted[i] = true;
		report("perm_storage: group '" + groups[i].name +
		       "' takes part in a parent cycle, loading it as a root");
		out.push_back(std::move(groups[i]));
	}

	return out;
}

// --- PermStore ---------------------------------------------------------------
PermStore::PermStore(plgdb::Database& db) : m_db(db) {}

PermStore::~PermStore() = default;

void PermStore::ReportError(std::string_view message) const {
	if (onError) {
		onError(message);
	}
}

void PermStore::EnsureSchema() {
	for (const DdlStatement& statement : kCreateTables) {
		const plgdb::Result result =
			m_db.QuerySync(std::string(Pick(m_db.GetDriver(), statement.mysql, statement.sqlite)), {});
		if (!result.Ok()) {
			throw std::runtime_error("perm_storage: creating the perm_* tables failed: " + result.Error());
		}
	}
}

// --- group snapshot ----------------------------------------------------------
GroupPlan* PermStore::FindGroup(std::string_view name) {
	for (GroupPlan& group : m_groups) {
		if (group.name == name) {
			return &group;
		}
	}
	return nullptr;
}

void PermStore::ResortGroups() {
	// The in-memory snapshot is scratch state; anomalies (a group whose parent is not saved
	// yet) are reported when the snapshot is read back from the database instead.
	m_groups = SortGroupsByParent(std::move(m_groups));
}

const std::vector<GroupPlan>& PermStore::LoadGroupsSync() {
	const Result groups = m_db.QuerySync(std::string(kSelectGroups), {});
	if (!groups.Ok()) {
		ReportError("perm_storage: loading the groups failed: " + groups.Error());
		return m_groups;
	}
	const Result permissions = m_db.QuerySync(std::string(kSelectGroupPermissions), {});
	if (!permissions.Ok()) {
		ReportError("perm_storage: loading the group permissions failed: " + permissions.Error());
		return m_groups;
	}
	const Result options = m_db.QuerySync(std::string(kSelectGroupOptions), {});
	if (!options.Ok()) {
		ReportError("perm_storage: loading the group options failed: " + options.Error());
		return m_groups;
	}
	ApplyGroupSnapshot(groups, permissions, options);
	return m_groups;
}

// Recovery walk for a degraded start (see the header): the same statements as
// EnsureSchema()/LoadGroupsSync(), chained through the worker thread. GroupReload::step counts
// the DDL statements first, then the three SELECTs, then the rebuild of the snapshot.
void PermStore::ReloadAsync(std::function<void(bool)> done) {
	auto state = std::make_shared<GroupReload>();
	state->done = std::move(done);
	StepGroupReload(state);
}

void PermStore::StepGroupReload(const std::shared_ptr<GroupReload>& state) {
	if (state->step < kCreateTableCount) {
		const DdlStatement& statement = kCreateTables[state->step];
		++state->step;
		Query(std::string(Pick(m_db.GetDriver(), statement.mysql, statement.sqlite)), {},
		      [this, state](const Result& result) {
			      if (result.Ok()) {
				      StepGroupReload(state);
				      return;
			      }
			      state->done(false); // silent on purpose: the caller logs state changes only
		      });
		return;
	}
	if (state->step == kCreateTableCount) {
		++state->step;
		Query(std::string(kSelectGroups), {}, [this, state](const Result& result) {
			state->groups = result;
			if (result.Ok()) {
				StepGroupReload(state);
			} else {
				state->done(false);
			}
		});
		return;
	}
	if (state->step == kCreateTableCount + 1) {
		++state->step;
		Query(std::string(kSelectGroupPermissions), {}, [this, state](const Result& result) {
			state->permissions = result;
			if (result.Ok()) {
				StepGroupReload(state);
			} else {
				state->done(false);
			}
		});
		return;
	}
	if (state->step == kCreateTableCount + 2) {
		++state->step;
		Query(std::string(kSelectGroupOptions), {}, [this, state](const Result& result) {
			state->options = result;
			if (result.Ok()) {
				StepGroupReload(state);
			} else {
				state->done(false);
			}
		});
		return;
	}

	ApplyGroupSnapshot(state->groups, state->permissions, state->options);
	state->done(m_groupsLoaded);
}

// Parses three SELECT results (the queries themselves are issued by the callers: synchronously
// at startup, asynchronously during a recovery walk) into the group snapshot. An unexpected
// column layout is reported and leaves both the snapshot and GroupsLoaded() alone.
void PermStore::ApplyGroupSnapshot(const Result& groups, const Result& permissions, const Result& options) {
	std::map<std::string, std::vector<std::string>, std::less<>> permissionsByGroup;
	if (const auto groupCol = permissions.ColumnIndex("group_name");
	    groupCol && permissions.ColumnIndex("perm") && permissions.ColumnIndex("state")) {
		const auto permCol = *permissions.ColumnIndex("perm");
		const auto stateCol = *permissions.ColumnIndex("state");
		for (size_t row = 0; row < permissions.RowCount(); ++row) {
			const std::string perm = permissions.GetString(row, permCol);
			const bool allowed = permissions.GetInt(row, stateCol, 1) != 0;
			permissionsByGroup[permissions.GetString(row, *groupCol)].push_back((allowed ? "" : "-") + perm);
		}
	}

	std::map<std::string, std::vector<UserCookie>, std::less<>> optionsByGroup;
	if (const auto groupCol = options.ColumnIndex("group_name");
	    groupCol && options.ColumnIndex("option_name") && options.ColumnIndex("value_type") &&
	    options.ColumnIndex("option_value")) {
		const auto nameCol = *options.ColumnIndex("option_name");
		const auto typeCol = *options.ColumnIndex("value_type");
		const auto valueCol = *options.ColumnIndex("option_value");
		for (size_t row = 0; row < options.RowCount(); ++row) {
			const std::string type = options.GetString(row, typeCol);
			const std::optional<Value> value =
				type.empty() ? std::nullopt
				             : Value::Decode(type.front(), options.GetString(row, valueCol));
			if (!value) {
				ReportError("perm_storage: skipping group option with an unsupported stored type");
				continue;
			}
			optionsByGroup[options.GetString(row, *groupCol)].push_back(
				UserCookie{options.GetString(row, nameCol), *value});
		}
	}

	std::vector<GroupPlan> plans;
	plans.reserve(groups.RowCount());
	const auto nameCol = groups.ColumnIndex("name");
	const auto priorityCol = groups.ColumnIndex("priority");
	const auto parentCol = groups.ColumnIndex("parent");
	if (!nameCol || !priorityCol || !parentCol) {
		ReportError("perm_storage: unexpected column layout of perm_groups");
		return;
	}
	for (size_t row = 0; row < groups.RowCount(); ++row) {
		GroupPlan plan;
		plan.name = groups.GetString(row, *nameCol);
		plan.priority = groups.GetInt(row, *priorityCol, 0);
		plan.parent = groups.GetString(row, *parentCol);
		if (const auto it = permissionsByGroup.find(plan.name); it != permissionsByGroup.end()) {
			plan.permissions = it->second;
		}
		if (const auto it = optionsByGroup.find(plan.name); it != optionsByGroup.end()) {
			plan.options = it->second;
		}
		plans.push_back(std::move(plan));
	}

	m_groups = SortGroupsByParent(std::move(plans),
	                              [this](std::string_view message) { ReportError(message); });
	m_groupsLoaded = true;
}

// --- asynchronous statement helpers ------------------------------------------
void PermStore::Query(std::string sql, Params params, plgdb::Callback callback) {
	m_db.Query(std::move(sql), std::move(params), std::move(callback));
}

void PermStore::Exec(std::string_view sql, Params params) {
	m_db.Query(std::string(sql), std::move(params), [this](const Result& result) {
		try {
			if (!result.Ok()) {
				ReportError("perm_storage: write failed: " + result.Error());
			}
		} catch (...) {
			// Never let an exception escape into plgdb::Database::Poll().
		}
	});
}

void PermStore::ExecTransaction(std::vector<Statement> statements) {
	m_db.Transaction(std::move(statements), [this](const Result& result) {
		try {
			if (!result.Ok()) {
				ReportError("perm_storage: transaction failed: " + result.Error());
			}
		} catch (...) {
		}
	});
}

// --- user load ---------------------------------------------------------------
PermStore::LoadRequest PermStore::RequestUserLoad(uint64_t steamid, std::string username, bool offline) {
	UserState& state = m_users[steamid];
	if (state.phase == UserState::Phase::Loaded) {
		return LoadRequest::AlreadyLoaded;
	}
	if (state.phase == UserState::Phase::Loading) {
		return LoadRequest::AlreadyLoading;
	}
	m_retries.erase(steamid); // a fresh request supersedes a pending retry
	StartUserLoad(steamid, std::move(username), offline, /*attempt=*/0);
	return LoadRequest::Started;
}

void PermStore::SetRetryPolicy(int attempts, int delaySeconds) {
	m_maxAttempts = attempts < 1 ? 1 : attempts;
	m_retryDelaySec = delaySeconds < 0 ? 0 : delaySeconds;
}

void PermStore::RunRetries() {
	if (m_retries.empty()) {
		return;
	}
	const int64_t now = Now();
	for (auto it = m_retries.begin(); it != m_retries.end();) {
		const uint64_t steamid = it->first;
		if (m_users[steamid].phase != UserState::Phase::None) {
			it = m_retries.erase(it); // loaded, or loading again
			continue;
		}
		if (it->second.nextAttemptAt > now) {
			++it;
			continue;
		}
		const std::string username = it->second.username;
		const bool offline = it->second.offline;
		const int attempt = it->second.attempt;
		it = m_retries.erase(it);
		StartUserLoad(steamid, username, offline, attempt);
	}
}

void PermStore::StartUserLoad(uint64_t steamid, std::string username, bool offline, int attempt) {
	UserState& state = m_users[steamid];
	if (state.phase != UserState::Phase::None) {
		return; // loaded or loading already
	}
	state.phase = UserState::Phase::Loading;
	const uint64_t generation = ++state.generation;

	auto pending = std::make_shared<PendingUserLoad>();
	pending->steamid = steamid;
	pending->generation = generation;
	pending->username = std::move(username);
	pending->offline = offline;
	pending->attempt = attempt;
	pending->remaining = 4;

	const int64_t steam = static_cast<int64_t>(steamid);
	const int64_t now = Now();

	Query(std::string(kSelectUser), {steam}, [this, pending](const Result& result) {
		try {
			if (pending->failed) {
				StepLoad(pending);
				return;
			}
			if (!result.Ok()) {
				FailLoad(pending, "loading the user row", result.Error());
				return;
			}
			if (result.RowCount() > 0) {
				pending->known = true;
				if (const auto col = result.ColumnIndex("immunity")) {
					pending->immunity = result.GetInt(0, *col, -1);
				}
			}
			StepLoad(pending);
		} catch (...) {
			FailLoad(pending, "loading the user row", "unexpected exception");
		}
	});

	Query(std::string(kSelectUserPermissions), {steam}, [this, pending, now](const Result& result) {
		try {
			if (pending->failed) {
				StepLoad(pending);
				return;
			}
			if (!result.Ok()) {
				FailLoad(pending, "loading the user permissions", result.Error());
				return;
			}
			const auto permCol = result.ColumnIndex("perm");
			const auto stateCol = result.ColumnIndex("state");
			const auto expiresCol = result.ColumnIndex("expires_at");
			if (!permCol || !stateCol || !expiresCol) {
				FailLoad(pending, "loading the user permissions", "unexpected column layout");
				return;
			}
			for (size_t row = 0; row < result.RowCount(); ++row) {
				const int64_t expires = result.GetInt(row, *expiresCol, 0);
				const std::string perm = result.GetString(row, *permCol);
				if (expires != 0 && expires <= now) {
					pending->expiredPermissions.push_back(perm); // cleaned up below
					continue;
				}
				pending->permissions.push_back(
					UserPermission{perm, result.GetInt(row, *stateCol, 1) == 0, expires});
			}
			StepLoad(pending);
		} catch (...) {
			FailLoad(pending, "loading the user permissions", "unexpected exception");
		}
	});

	Query(std::string(kSelectUserGroups), {steam}, [this, pending, now](const Result& result) {
		try {
			if (pending->failed) {
				StepLoad(pending);
				return;
			}
			if (!result.Ok()) {
				FailLoad(pending, "loading the user groups", result.Error());
				return;
			}
			const auto groupCol = result.ColumnIndex("group_name");
			const auto expiresCol = result.ColumnIndex("expires_at");
			if (!groupCol || !expiresCol) {
				FailLoad(pending, "loading the user groups", "unexpected column layout");
				return;
			}
			for (size_t row = 0; row < result.RowCount(); ++row) {
				const int64_t expires = result.GetInt(row, *expiresCol, 0);
				const std::string group = result.GetString(row, *groupCol);
				if (expires != 0 && expires <= now) {
					pending->expiredGroups.push_back(group);
					continue;
				}
				pending->groups.push_back(UserGroupMembership{group, expires});
			}
			StepLoad(pending);
		} catch (...) {
			FailLoad(pending, "loading the user groups", "unexpected exception");
		}
	});

	Query(std::string(kSelectUserCookies), {steam}, [this, pending](const Result& result) {
		try {
			if (pending->failed) {
				StepLoad(pending);
				return;
			}
			if (!result.Ok()) {
				FailLoad(pending, "loading the user cookies", result.Error());
				return;
			}
			const auto nameCol = result.ColumnIndex("name");
			const auto typeCol = result.ColumnIndex("value_type");
			const auto valueCol = result.ColumnIndex("cookie_value");
			if (!nameCol || !typeCol || !valueCol) {
				FailLoad(pending, "loading the user cookies", "unexpected column layout");
				return;
			}
			for (size_t row = 0; row < result.RowCount(); ++row) {
				const std::string type = result.GetString(row, *typeCol);
				const std::optional<Value> value =
					type.empty() ? std::nullopt : Value::Decode(type.front(), result.GetString(row, *valueCol));
				if (!value) {
					ReportError("perm_storage: skipping user cookie with an unsupported stored type");
					continue;
				}
				pending->cookies.push_back(UserCookie{result.GetString(row, *nameCol), *value});
			}
			StepLoad(pending);
		} catch (...) {
			FailLoad(pending, "loading the user cookies", "unexpected exception");
		}
	});

	// Queued last on purpose: `known` above must reflect the row as it was before this
	// request recorded the freshly seen name and timestamp.
	Exec(Pick(m_db.GetDriver(), kUpsertUserSeenMySQL, kUpsertUserSeenSQLite),
	     {steam, pending->username, now, now});
}

void PermStore::StepLoad(const std::shared_ptr<PendingUserLoad>& pending) {
	if (--pending->remaining > 0) {
		return;
	}
	FinishUserLoad(*pending);
}

void PermStore::FailLoad(const std::shared_ptr<PendingUserLoad>& pending, std::string_view what,
                         std::string_view error) {
	pending->failed = true;
	ReportError("perm_storage: " + std::string(what) + " failed for " + std::to_string(pending->steamid) +
	            ": " + std::string(error));
	StepLoad(pending);
}

void PermStore::FinishUserLoad(PendingUserLoad& pending) {
	// Rows that already expired: the core only removes them from memory (its expiration
	// timers never reach the storage callbacks), so the database is cleaned up here.
	const int64_t steam = static_cast<int64_t>(pending.steamid);
	for (const std::string& perm : pending.expiredPermissions) {
		Exec(kDeleteUserPermission, {steam, perm});
	}
	for (const std::string& group : pending.expiredGroups) {
		Exec(kDeleteUserGroup, {steam, group});
	}

	const auto it = m_users.find(pending.steamid);
	if (it == m_users.end() || it->second.generation != pending.generation) {
		return; // the user was deleted (or reloaded) while the SELECTs were in flight
	}
	if (pending.failed) {
		// Stay unloaded; a later request retries, and RunRetries() re-issues this one after
		// a delay (transient database failures must not strand a player without permissions
		// for the whole session).
		it->second.phase = UserState::Phase::None;
		if (pending.attempt + 1 < m_maxAttempts) {
			Retry& retry = m_retries[pending.steamid];
			retry.username = pending.username;
			retry.offline = pending.offline;
			retry.attempt = pending.attempt + 1;
			retry.nextAttemptAt = Now() + m_retryDelaySec;
		}
		return;
	}

	UserPlan plan;
	plan.steamid = pending.steamid;
	plan.username = pending.username;
	plan.offline = pending.offline;
	plan.known = pending.known;
	plan.immunity = pending.immunity;
	plan.permissions = std::move(pending.permissions);
	plan.groups = std::move(pending.groups);
	plan.cookies = std::move(pending.cookies);

	// Loaded before the callback: the glue re-issues LoadUser() from inside it, and that
	// dispatch must see the user as already loaded (so it is not cancelled again).
	it->second.phase = UserState::Phase::Loaded;
	if (onUserLoaded) {
		try {
			onUserLoaded(plan);
		} catch (...) {
			ReportError("perm_storage: an onUserLoaded handler threw an exception");
		}
	}
}

void PermStore::ForgetUser(uint64_t steamid) {
	m_retries.erase(steamid); // a deleted user must not be loaded again by a pending retry
	const auto it = m_users.find(steamid);
	if (it == m_users.end()) {
		return;
	}
	++it->second.generation; // anything in flight for this steamid is now stale
	it->second.phase = UserState::Phase::None;
}

bool PermStore::IsUserLoaded(uint64_t steamid) const {
	const auto it = m_users.find(steamid);
	return it != m_users.end() && it->second.phase == UserState::Phase::Loaded;
}

// --- user writers ------------------------------------------------------------
void PermStore::SaveUserImmunity(uint64_t steamid, int64_t immunity) {
	const int64_t now = Now();
	Exec(Pick(m_db.GetDriver(), kUpsertUserImmunityMySQL, kUpsertUserImmunitySQLite),
	     {static_cast<int64_t>(steamid), immunity, now, now});
}

void PermStore::SaveUserPermission(uint64_t steamid, const PermChange& change) {
	const int64_t steam = static_cast<int64_t>(steamid);
	const std::string_view upsert = Pick(m_db.GetDriver(), kUpsertUserPermissionMySQL, kUpsertUserPermissionSQLite);

	switch (change.action) {
	case PermAction::Remove:
		Exec(kDeleteUserPermission, {steam, change.perm});
		return;
	case PermAction::ReplaceToWildcard: {
		// The plain permission ("a") was replaced by its wildcard ("a.*") in the core.
		std::vector<Statement> statements;
		statements.push_back({std::string(kDeleteUserPermission),
		                      {steam, std::string(WildcardRootOf(change.perm))}});
		statements.push_back({std::string(upsert),
		                      {steam, change.perm, static_cast<int64_t>(change.denied ? 0 : 1),
		                       change.expiresAt}});
		ExecTransaction(std::move(statements));
		return;
	}
	case PermAction::Add:
	case PermAction::Replace:
		Exec(upsert, {steam, change.perm, static_cast<int64_t>(change.denied ? 0 : 1), change.expiresAt});
		return;
	}
}

void PermStore::SaveUserGroup(uint64_t steamid, const GroupChange& change) {
	const int64_t steam = static_cast<int64_t>(steamid);
	switch (change.action) {
	case GroupAction::Remove:
		Exec(kDeleteUserGroup, {steam, change.group});
		return;
	case GroupAction::Add:
	case GroupAction::Replace:
		Exec(Pick(m_db.GetDriver(), kUpsertUserGroupMySQL, kUpsertUserGroupSQLite),
		     {steam, change.group, change.expiresAt});
		return;
	}
}

void PermStore::SaveUserCookie(uint64_t steamid, std::string_view name, const Value& value) {
	Exec(Pick(m_db.GetDriver(), kUpsertUserCookieMySQL, kUpsertUserCookieSQLite),
	     {static_cast<int64_t>(steamid), std::string(name),
	      std::string(1, static_cast<char>(value.kind)), value.Encode()});
}

// --- group writers -----------------------------------------------------------
void PermStore::ApplyGroupPermission(GroupPlan& group, const PermChange& change) {
	const auto samePerm = [&change](const std::string& line) {
		return StripDenyPrefix(line) == change.perm;
	};
	if (change.action == PermAction::Remove) {
		std::erase_if(group.permissions, samePerm);
		return;
	}
	if (change.action == PermAction::ReplaceToWildcard) {
		const std::string_view root = WildcardRootOf(change.perm);
		std::erase_if(group.permissions,
		              [root](const std::string& line) { return StripDenyPrefix(line) == root; });
	} else {
		std::erase_if(group.permissions, samePerm);
	}
	group.permissions.push_back((change.denied ? "-" : "") + change.perm);
}

void PermStore::ApplyGroupOption(GroupPlan& group, const std::string& name, const Value& value) {
	for (UserCookie& option : group.options) {
		if (option.name == name) {
			option.value = value;
			return;
		}
	}
	group.options.push_back(UserCookie{name, value});
}

void PermStore::SaveGroup(std::string name, int64_t priority, std::string parent,
                          std::vector<std::string> permissions) {
	const std::string_view upsert = Pick(m_db.GetDriver(), kUpsertGroupMySQL, kUpsertGroupSQLite);
	const std::string_view upsertPerm =
		Pick(m_db.GetDriver(), kUpsertGroupPermissionMySQL, kUpsertGroupPermissionSQLite);

	std::vector<Statement> statements;
	statements.push_back({std::string(upsert),
	                      {name, priority, parent.empty() ? plgdb::Value{} : plgdb::Value{parent}}});
	statements.push_back({std::string(kDeleteGroupPermissions), {name}});
	for (const std::string& line : permissions) {
		const std::string_view perm = StripDenyPrefix(line);
		statements.push_back({std::string(upsertPerm),
		                      {name, std::string(perm), static_cast<int64_t>(IsDeniedLine(line) ? 0 : 1)}});
	}

	GroupPlan plan;
	plan.name = name;
	plan.priority = priority;
	plan.parent = std::move(parent);
	plan.permissions = std::move(permissions);
	if (GroupPlan* existing = FindGroup(name)) {
		// The create event replaces the permissions but never the options (the statement
		// list above leaves perm_group_options alone), so the snapshot keeps them too.
		plan.options = std::move(existing->options);
		*existing = std::move(plan);
	} else {
		m_groups.push_back(std::move(plan));
	}
	ResortGroups();

	ExecTransaction(std::move(statements));
}

void PermStore::DropGroup(std::string_view name) {
	std::vector<Statement> statements;
	statements.push_back({std::string(kDeleteGroupPermissions), {std::string(name)}});
	statements.push_back({std::string(kDeleteGroupOptions), {std::string(name)}});
	// DeleteGroup() in the core detaches the group from every user without dispatching the
	// user group storage callbacks, so the memberships are removed here.
	statements.push_back({std::string(kDeleteGroupMemberships), {std::string(name)}});
	statements.push_back({std::string(kClearGroupParent), {std::string(name)}});
	statements.push_back({std::string(kDeleteGroup), {std::string(name)}});

	std::erase_if(m_groups, [name](const GroupPlan& group) { return group.name == name; });
	for (GroupPlan& group : m_groups) {
		if (group.parent == name) {
			group.parent.clear();
		}
	}
	ResortGroups();

	ExecTransaction(std::move(statements));
}

void PermStore::SaveGroupParent(std::string_view child, std::string_view parent) {
	if (GroupPlan* group = FindGroup(child)) {
		group->parent = std::string(parent);
		ResortGroups();
	}
	Exec(kSetGroupParent, {parent.empty() ? plgdb::Value{} : plgdb::Value{std::string(parent)},
	                       std::string(child)});
}

void PermStore::SaveGroupPermission(std::string_view group, const PermChange& change) {
	const std::string_view upsert =
		Pick(m_db.GetDriver(), kUpsertGroupPermissionMySQL, kUpsertGroupPermissionSQLite);
	const std::string groupName(group);

	if (GroupPlan* plan = FindGroup(group)) {
		ApplyGroupPermission(*plan, change);
	}

	switch (change.action) {
	case PermAction::Remove:
		Exec(kDeleteGroupPermission, {groupName, change.perm});
		return;
	case PermAction::ReplaceToWildcard: {
		std::vector<Statement> statements;
		statements.push_back(
			{std::string(kDeleteGroupPermission), {groupName, std::string(WildcardRootOf(change.perm))}});
		statements.push_back(
			{std::string(upsert), {groupName, change.perm, static_cast<int64_t>(change.denied ? 0 : 1)}});
		ExecTransaction(std::move(statements));
		return;
	}
	case PermAction::Add:
	case PermAction::Replace:
		Exec(upsert, {groupName, change.perm, static_cast<int64_t>(change.denied ? 0 : 1)});
		return;
	}
}

void PermStore::SaveGroupOption(std::string_view group, std::string_view name, const Value& value) {
	if (GroupPlan* plan = FindGroup(group)) {
		ApplyGroupOption(*plan, std::string(name), value);
	}
	Exec(Pick(m_db.GetDriver(), kUpsertGroupOptionMySQL, kUpsertGroupOptionSQLite),
	     {std::string(group), std::string(name), std::string(1, static_cast<char>(value.kind)),
	      value.Encode()});
}

} // namespace perm_storage
