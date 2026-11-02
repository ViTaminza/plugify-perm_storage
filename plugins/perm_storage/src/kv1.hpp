// perm_storage — a read-only Valve KeyValues (KV1) reader for databases.cfg.
//
// perm_storage is pure Plugify + plgdb and self-contained (docs/PLUGIN_STANDARD.md §7): the
// databases.cfg file is read with this parser instead of a shim library's KeyValues reader. The file
// is plain KV1 (docs/PLUGIN_STANDARD.md §4):
//
//   "Databases"
//   {
//       "perm_storage"
//       {
//           "driver"    "mysql"
//           "host"      "127.0.0.1"
//           "port"      "3306"
//           "database"  "perm_storage"
//           "user"      "..."
//           "pass"      "..."
//       }
//   }
//
// The accepted grammar is the one the original port's KeyValues reader accepted:
//   * quoted ("key" "value") and unquoted (key value) tokens, nested "key" { ... } blocks, a key
//     may carry a value and a block,
//   * // line and /* */ block comments anywhere between tokens (including after a value),
//   * \" \\ \n \r \t escapes inside quoted strings; UTF-8 passes through as bytes,
//   * #base / #include / #define lines are skipped (no inclusion),
//   * an unterminated string, a missing '}' or a stray top-level '}' makes the file invalid
//     (nullopt) instead of returning a half-read tree.
// Like Valve's loader, a file that consists of a single top-level block is returned as that
// block, so ParseKv1() of the standard file returns the "Databases" node and its children are the
// sections (a file with only the "perm_storage" block is handled by db_config.cpp as well). Key
// lookup is case-insensitive.
//
// Header-only on purpose: src/db_config.cpp and tests/db_config_tests.cpp share the exact same
// parser, and neither target needs a shim library, an engine or an extra object file.

#pragma once

#include <cctype>
#include <charconv>
#include <cstddef>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace perm_storage {

namespace kv1_detail {

inline char Lower(char c) {
	return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
}

inline bool EqualNoCase(std::string_view a, std::string_view b) {
	if (a.size() != b.size()) {
		return false;
	}
	for (std::size_t i = 0; i < a.size(); ++i) {
		if (Lower(a[i]) != Lower(b[i])) {
			return false;
		}
	}
	return true;
}

inline std::string_view Trim(std::string_view text) {
	std::size_t begin = 0;
	std::size_t end = text.size();
	while (begin < end && std::isspace(static_cast<unsigned char>(text[begin]))) {
		++begin;
	}
	while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1]))) {
		--end;
	}
	return text.substr(begin, end - begin);
}

} // namespace kv1_detail

struct KvNode {
	std::string name;
	std::string value;
	bool hasValue = false;
	std::vector<KvNode> children;

	// First child with that name (case-insensitive), or nullptr.
	const KvNode* Find(std::string_view key) const {
		if (key.empty()) {
			return nullptr;
		}
		for (const KvNode& child : children) {
			if (kv1_detail::EqualNoCase(child.name, key)) {
				return &child;
			}
		}
		return nullptr;
	}

	// The child's value (an empty string is a value, like in Valve's KeyValues); `def` only when
	// the child is missing or has no value at all. An empty `key` reads this node's own value.
	std::string_view GetString(std::string_view key, std::string_view def = {}) const {
		if (key.empty()) {
			return hasValue ? std::string_view(value) : def;
		}
		const KvNode* found = Find(key);
		if (found == nullptr || !found->hasValue) {
			return def;
		}
		return found->value;
	}

	// The child's value as an int, with Valve's atoi fallback ("abc" -> 0); `def` when the child
	// is missing or its value is empty.
	int GetInt(std::string_view key, int def = 0) const {
		const KvNode* found = Find(key);
		return found != nullptr ? found->IntValue(def) : def;
	}

	// This node's own value as an int, with Valve's atoi fallback; `def` when the node has no value
	// or the value is empty (after trimming).
	int IntValue(int def = 0) const {
		if (!hasValue) {
			return def;
		}
		const std::string_view text = kv1_detail::Trim(value);
		if (text.empty()) {
			return def;
		}
		int number = 0;
		const auto result = std::from_chars(text.data(), text.data() + text.size(), number);
		if (result.ec != std::errc{}) {
			return std::atoi(std::string(text).c_str()); // Valve's atoi semantics
		}
		return number;
	}
};

namespace kv1_detail {

// The tokenizer and grammar of the original port's KeyValues reader, building a KvNode tree instead:
//
//   file  := node* EOF
//   node  := token [ token ] [ '{' node* '}' ]
//   token := '"' escaped '"' | raw
class Parser {
public:
	explicit Parser(std::string_view text) : text_(text) {
		// A UTF-8 BOM written by a Windows editor is not part of the first key.
		if (text_.size() >= 3 && static_cast<unsigned char>(text_[0]) == 0xEF &&
		    static_cast<unsigned char>(text_[1]) == 0xBB && static_cast<unsigned char>(text_[2]) == 0xBF) {
			pos_ = 3;
		}
	}

	bool Parse(KvNode& root) {
		ParseNodes(root, false);
		return !failed_;
	}

private:
	void SkipSpace() {
		while (pos_ < text_.size()) {
			const char c = text_[pos_];
			if (std::isspace(static_cast<unsigned char>(c))) {
				++pos_;
				continue;
			}
			if (c == '/' && pos_ + 1 < text_.size() && text_[pos_ + 1] == '/') {
				pos_ += 2;
				while (pos_ < text_.size() && text_[pos_] != '\n') {
					++pos_;
				}
				continue;
			}
			if (c == '/' && pos_ + 1 < text_.size() && text_[pos_ + 1] == '*') {
				pos_ += 2;
				while (pos_ + 1 < text_.size() && !(text_[pos_] == '*' && text_[pos_ + 1] == '/')) {
					++pos_;
				}
				pos_ = (pos_ + 1 < text_.size()) ? pos_ + 2 : text_.size();
				continue;
			}
			break;
		}
	}

	// One token; false at the end of the input or at a '}'.
	bool ReadToken(std::string& out) {
		SkipSpace();
		if (pos_ >= text_.size() || text_[pos_] == '}') {
			return false;
		}
		out.clear();
		if (text_[pos_] == '"') {
			++pos_;
			while (pos_ < text_.size()) {
				const char c = text_[pos_++];
				if (c == '\\' && pos_ < text_.size()) {
					const char esc = text_[pos_++];
					switch (esc) {
					case 'n': out.push_back('\n'); break;
					case 'r': out.push_back('\r'); break;
					case 't': out.push_back('\t'); break;
					case '"': out.push_back('"'); break;
					case '\\': out.push_back('\\'); break;
					default:
						out.push_back('\\');
						out.push_back(esc);
						break;
					}
					continue;
				}
				if (c == '"') {
					return true;
				}
				out.push_back(c);
			}
			failed_ = true; // unterminated string
			return true;
		}
		while (pos_ < text_.size()) {
			const char c = text_[pos_];
			if (std::isspace(static_cast<unsigned char>(c)) || c == '{' || c == '}' || c == '"') {
				break;
			}
			if (c == '/' && pos_ + 1 < text_.size() && (text_[pos_ + 1] == '/' || text_[pos_ + 1] == '*')) {
				break;
			}
			out.push_back(c);
			++pos_;
		}
		return true;
	}

	bool PeekIs(char c) {
		SkipSpace();
		return pos_ < text_.size() && text_[pos_] == c;
	}

	void ParseNodes(KvNode& parent, bool insideBlock) {
		while (true) {
			SkipSpace();
			if (pos_ >= text_.size()) {
				if (insideBlock) {
					failed_ = true; // missing '}'
				}
				return;
			}
			if (text_[pos_] == '}') {
				if (!insideBlock) {
					failed_ = true; // stray '}'
				}
				++pos_;
				return;
			}
			std::string key;
			if (!ReadToken(key)) {
				return;
			}
			if (!key.empty() && key[0] == '#') {
				// #base / #include / #define: no inclusion, skip the line.
				while (pos_ < text_.size() && text_[pos_] != '\n') {
					++pos_;
				}
				continue;
			}
			parent.children.push_back(KvNode{});
			// Filled through an index: the recursion below may reallocate parent.children.
			const std::size_t index = parent.children.size() - 1;
			parent.children[index].name = std::move(key);
			if (!PeekIs('{')) {
				std::string value;
				if (ReadToken(value)) {
					parent.children[index].value = std::move(value);
					parent.children[index].hasValue = true;
				}
			}
			if (PeekIs('{')) {
				++pos_;
				KvNode child = std::move(parent.children[index]);
				ParseNodes(child, true);
				parent.children[index] = std::move(child);
			}
		}
	}

	std::string_view text_;
	std::size_t pos_ = 0;
	bool failed_ = false;
};

} // namespace kv1_detail

// Parses a whole KV1 buffer. A single top-level block is returned as the root node, exactly like
// Valve's loader (so the standard databases.cfg yields the "Databases" node, whose children are
// the connection sections).
inline std::optional<KvNode> ParseKv1(std::string_view text) {
	KvNode root;
	kv1_detail::Parser parser(text);
	if (!parser.Parse(root)) {
		return std::nullopt;
	}
	if (root.children.size() == 1 && !root.children.front().children.empty()) {
		KvNode top = std::move(root.children.front());
		return top;
	}
	return root;
}

// The whole file, or nullopt when it cannot be opened. Kept separate from ParseKv1 so the caller
// can tell "not installed" (try the next candidate location) from "installed but malformed".
inline std::optional<std::string> ReadTextFile(const std::string& path) {
	if (path.empty()) {
		return std::nullopt;
	}
	std::ifstream file(path, std::ios::binary);
	if (!file) {
		return std::nullopt;
	}
	return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

} // namespace perm_storage
