#pragma once
// Engine-independent core of the "perm_storage" plugin: schema, value (de)serialization,
// SQL building and the user-load state machine. No Plugify / permissions headers here, so
// everything is unit-testable against plgdb alone (tests/perm_storage_tests.cpp); the
// plugin glue (plugin.cpp) turns the plans produced here into permissions:: calls.
//
// Contract (from ref/permissions/src, verified against the source):
//   * Every storage callback fires BEFORE the in-memory change and carries the pluginID of
//     the caller; the storage plugin ignores its own id because its replays (used while
//     loading a user / the startup group load) go through the same API.
//   * The user load request (OnUserRequest, dispatched by LoadUser) is asynchronous here:
//     the handler answers "not ready" (false -> DBNotReady) and the plugin re-issues
//     LoadUser(ownID, ..., dontBroadcast=false) once the plan has been applied, which is
//     what broadcasts OnUserLoaded to the connector.
//   * Permissions are stored as (perm, state) pairs, where the state column holds
//     1 = Allow / 0 = Disallow (the core's "-perm" prefix carries the same information in
//     memory); `expires_at` is an absolute unix timestamp, 0 = permanent. Rows whose
//     expires_at is in the past are skipped and deleted when the user is loaded.
//
// Threading: every method runs on the game thread (plgdb::Poll() callbacks included);
// nothing here blocks except EnsureSchema()/LoadGroupsSync(), which the plugin calls only
// during OnPluginStart.

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <plgdb/db.hpp>

namespace perm_storage {

// --- prefixed table names ----------------------------------------------------
inline constexpr std::string_view kTableUsers = "perm_users";
inline constexpr std::string_view kTableUserPermissions = "perm_user_permissions";
inline constexpr std::string_view kTableUserGroups = "perm_user_groups";
inline constexpr std::string_view kTableUserCookies = "perm_user_cookies";
inline constexpr std::string_view kTableGroups = "perm_groups";
inline constexpr std::string_view kTableGroupPermissions = "perm_group_permissions";
inline constexpr std::string_view kTableGroupOptions = "perm_group_options";

// --- tagged values -----------------------------------------------------------
// The subset of plg::any that is persisted and restored: bool, int64, double and string.
// The `type` column stores Kind as a one-character tag, so a stored value round-trips to
// the same plg::any alternative.
struct Value {
	enum class Kind : char {
		Bool = 'b',
		Int = 'i',
		Double = 'd',
		String = 's',
	};

	Kind kind = Kind::String;
	bool boolean = false;
	int64_t integer = 0;
	double real = 0.0;
	std::string text;

	static Value MakeBool(bool v);
	static Value MakeInt(int64_t v);
	static Value MakeDouble(double v);
	static Value MakeString(std::string v);

	// Text form stored in the value column (doubles use the shortest representation that
	// round-trips exactly; strings are stored verbatim and are binary-safe).
	std::string Encode() const;
	// Parses a stored (type, text) pair; nullopt when the tag or the payload is invalid.
	static std::optional<Value> Decode(char type, std::string_view encoded);

	friend bool operator==(const Value& a, const Value& b) {
		if (a.kind != b.kind) {
			return false;
		}
		switch (a.kind) {
		case Kind::Bool: return a.boolean == b.boolean;
		case Kind::Int: return a.integer == b.integer;
		case Kind::Double: return a.real == b.real;
		case Kind::String: return a.text == b.text;
		}
		return false;
	}
};

// --- change descriptions (storage callbacks -> writers) ----------------------
enum class PermAction {
	Add,
	Remove,
	Replace,
	ReplaceToWildcard,
};

struct PermChange {
	PermAction action = PermAction::Add;
	std::string perm;      // permission line, always without the '-' deny prefix
	bool denied = false;   // Allow/Disallow, meaningful for Add/Replace/ReplaceToWildcard
	int64_t expiresAt = 0; // absolute unix expiry; 0 = permanent (user permissions only)
};

enum class GroupAction {
	Add,
	Remove,
	Replace,
};

struct GroupChange {
	GroupAction action = GroupAction::Add;
	std::string group;
	int64_t expiresAt = 0;
};

// --- load plans --------------------------------------------------------------
struct UserPermission {
	std::string perm; // without the '-' deny prefix
	bool denied = false;
	int64_t expiresAt = 0;
};

struct UserGroupMembership {
	std::string group;
	int64_t expiresAt = 0;
};

struct UserCookie {
	std::string name;
	Value value;
};

// Everything needed to recreate one user in the permissions core.
struct UserPlan {
	uint64_t steamid = 0;
	std::string username; // from the load request (recently seen name)
	bool offline = false;
	bool known = false;   // false: no perm_users row, the core gets a default user
	int64_t immunity = -1;
	std::vector<UserGroupMembership> groups;
	std::vector<UserPermission> permissions;
	std::vector<UserCookie> cookies;
};

// One group with its permissions and options.
struct GroupPlan {
	std::string name;
	int64_t priority = 0;
	std::string parent; // empty = none
	std::vector<std::string> permissions; // lines as the core stores them, '-' = denied
	std::vector<UserCookie> options;      // options reuse the cookie value shape
};

// Orders groups so that every parent precedes its children; the plans themselves are not
// modified (a parent that does not exist in the list is treated as a root, and members of a
// parent cycle are emitted in input order). Anomalies are reported through the callback; the
// caller decides what to do with a parent that does not exist in the core.
std::vector<GroupPlan> SortGroupsByParent(std::vector<GroupPlan> groups,
                                          const std::function<void(std::string_view)>& onError = {});

// --- store -------------------------------------------------------------------
class PermStore {
public:
	explicit PermStore(plgdb::Database& db);
	~PermStore();

	PermStore(const PermStore&) = delete;
	PermStore& operator=(const PermStore&) = delete;

	// Creates every perm_* table if missing. Blocking; startup only.
	// Throws std::runtime_error when a statement fails.
	void EnsureSchema();

	// Synchronous full read of the groups (blocking; startup only), parent-first ordered.
	// Replaces the in-memory snapshot returned by Groups().
	const std::vector<GroupPlan>& LoadGroupsSync();

	// In-memory snapshot of the groups, kept up to date by the Save*/Drop* writers below.
	// Used to (re)create groups in the core without touching the database again.
	const std::vector<GroupPlan>& Groups() const { return m_groups; }

	// --- user loads ----------------------------------------------------------
	enum class LoadRequest {
		Started,        // async SELECTs queued; the plan is emitted from Poll()
		AlreadyLoading, // a load for this steamid is in flight
		AlreadyLoaded,  // the plan has been applied already
	};

	// Starts (or reports) an asynchronous load of one user. Safe to call repeatedly.
	LoadRequest RequestUserLoad(uint64_t steamid, std::string username, bool offline);

	// The user is gone from the core (DeleteUser): forget the cached state so the next
	// request loads it again. A load still in flight is discarded when it lands.
	void ForgetUser(uint64_t steamid);

	// Re-issues loads that failed with a database error, after a delay, a limited number of
	// times (see SetRetryPolicy). Call from the plugin update.
	void RunRetries();

	// Retry policy for failed loads (attempts includes the first try; delay in seconds).
	void SetRetryPolicy(int attempts, int delaySeconds);

	bool IsUserLoaded(uint64_t steamid) const;

	// --- writers (all asynchronous, errors go to onError) --------------------
	void SaveUserImmunity(uint64_t steamid, int64_t immunity);
	void SaveUserPermission(uint64_t steamid, const PermChange& change);
	void SaveUserGroup(uint64_t steamid, const GroupChange& change);
	void SaveUserCookie(uint64_t steamid, std::string_view name, const Value& value);

	void SaveGroup(std::string name, int64_t priority, std::string parent,
	               std::vector<std::string> permissions);
	void DropGroup(std::string_view name);
	void SaveGroupParent(std::string_view child, std::string_view parent);
	void SaveGroupPermission(std::string_view group, const PermChange& change);
	void SaveGroupOption(std::string_view group, std::string_view name, const Value& value);

	// --- hooks (called on the game thread) -----------------------------------
	// A loaded user, right after the internal state switched to "loaded".
	std::function<void(const UserPlan&)> onUserLoaded;
	// SQL failures, rejected values and lost results. Never empty in the plugin.
	std::function<void(std::string_view)> onError;

private:
	struct UserState {
		enum class Phase : uint8_t { None, Loading, Loaded };
		Phase phase = Phase::None;
		uint64_t generation = 0; // invalidates results of loads for deleted/replaced users
	};

	struct PendingUserLoad {
		uint64_t steamid = 0;
		uint64_t generation = 0;
		std::string username;
		bool offline = false;
		int attempt = 0;   // 0 = first try; raised by the retry queue
		int remaining = 0; // outstanding SELECTs
		bool failed = false;
		bool known = false;
		int64_t immunity = -1;
		std::vector<UserPermission> permissions;
		std::vector<UserGroupMembership> groups;
		std::vector<UserCookie> cookies;
		std::vector<std::string> expiredPermissions; // deleted from the database on completion
		std::vector<std::string> expiredGroups;
	};

	void ReportError(std::string_view message) const;
	void StartUserLoad(uint64_t steamid, std::string username, bool offline, int attempt);
	void StepLoad(const std::shared_ptr<PendingUserLoad>& pending);
	void FailLoad(const std::shared_ptr<PendingUserLoad>& pending, std::string_view what,
	              std::string_view error);
	void FinishUserLoad(PendingUserLoad& pending);

	// Snapshot maintenance, mirroring what the writers do to the database.
	GroupPlan* FindGroup(std::string_view name);
	void ResortGroups();
	void ApplyGroupPermission(GroupPlan& group, const PermChange& change);
	void ApplyGroupOption(GroupPlan& group, const std::string& name, const Value& value);

	void Query(std::string sql, plgdb::Params params, plgdb::Callback callback);
	void Exec(std::string_view sql, plgdb::Params params);
	void ExecTransaction(std::vector<plgdb::Statement> statements);

	struct Retry {
		std::string username;
		bool offline = false;
		int attempt = 0;
		int64_t nextAttemptAt = 0;
	};

	plgdb::Database& m_db;
	std::vector<GroupPlan> m_groups;
	std::map<uint64_t, UserState> m_users;
	std::map<uint64_t, Retry> m_retries;
	int m_maxAttempts = 3;
	int m_retryDelaySec = 5;
};

} // namespace perm_storage
