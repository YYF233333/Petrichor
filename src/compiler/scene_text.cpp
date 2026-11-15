#include "compiler/scene_text.h"

#include "compiler/lexer.h"

#include <algorithm>
#include <optional>
#include <tuple>

namespace petrichor::compiler {

namespace {

// A value of the engine's text format, as far as the names in it go: the
// tokens are those of VariantParser::get_token(), without building the value
// (core/variant/variant_parser.cpp:160-505; ed1daf0).
struct Value {
	enum class Kind {
		String, // Also a StringName, &"…" or @"…".
		Array,
		Dictionary,
		Call, // A constructor such as NodePath("…"), Array[int]([…]) or SubResource("…").
		Other,
	};
	Kind kind = Kind::Other;
	// A string's text between its quotes; anything else whole.
	uint32_t begin = 0;
	uint32_t end = 0;
	// An Array's elements, a Dictionary's keys and values one after another,
	// a call's arguments.
	std::vector<Value> items;
};

// `key = value` after a section's header (variant_parser.cpp:1910-1957).
struct Entry {
	uint32_t begin = 0; // The key as written, quotes included.
	uint32_t end = 0;
	std::string key; // As the engine reads it.
	Value value;
};

// A `[tag field=value …]` header and the entries after it, up to the next
// (variant_parser.cpp:1750-1867).
struct Section {
	std::string tag;
	uint32_t begin = 0;
	std::vector<std::pair<std::string, Value>> fields;
	std::vector<Entry> entries;
};

bool is_space(char p_char) {
	return static_cast<unsigned char>(p_char) <= 32;
}

// What ends a bare token, such as a number, an identifier or a color.
bool ends_word(char p_char) {
	return is_space(p_char) || std::string("[]{}(),:\"=;").find(p_char) != std::string::npos;
}

class Reader {
	const std::string &text;
	size_t at = 0;

	bool fail(const std::string &p_error) {
		if (error.empty()) {
			error = p_error;
			error_at = uint32_t(std::min(at, text.size()));
		}
		at = text.size();
		return false;
	}

	char peek() const { return at < text.size() ? text[at] : '\0'; }

	// Whitespace and `;` comments, which run to the end of the line
	// (variant_parser.cpp:215-229).
	void skip_space() {
		while (at < text.size()) {
			if (text[at] == ';') {
				const size_t line_end = text.find('\n', at);
				at = line_end == std::string::npos ? text.size() : line_end + 1;
			} else if (is_space(text[at])) {
				at++;
			} else {
				return;
			}
		}
	}

	std::string word() {
		const size_t begin = at;
		while (at < text.size() && !ends_word(text[at])) {
			at++;
		}
		return text.substr(begin, at - begin);
	}

	// At the opening quote.
	Value string() {
		Value value;
		value.kind = Value::Kind::String;
		value.begin = uint32_t(++at);
		while (at < text.size() && text[at] != '"') {
			at += text[at] == '\\' ? 2 : 1;
		}
		if (at >= text.size()) {
			fail("a string is not closed");
			return value;
		}
		value.end = uint32_t(at++);
		return value;
	}

	// Values up to `p_close`, apart by commas, or by colons in a Dictionary
	// and in Object(…) of format 2.
	void items(Value &r_value, char p_close) {
		at++;
		while (true) {
			skip_space();
			if (peek() == p_close) {
				at++;
				return;
			}
			if (at >= text.size()) {
				fail("a bracket is not closed");
				return;
			}
			r_value.items.push_back(value());
			skip_space();
			if (peek() == ',' || peek() == ':') {
				at++;
			} else if (peek() != p_close) {
				fail("expected a comma");
				return;
			}
		}
	}

public:
	std::string error;
	uint32_t error_at = 0;

	explicit Reader(const std::string &p_text) :
			text(p_text) {}

	Value value() {
		skip_space();
		const char first = peek();
		if (first == '"' || ((first == '&' || first == '@') && at + 1 < text.size() && text[at + 1] == '"')) {
			at += first == '"' ? 0 : 1;
			return string();
		}
		Value value;
		value.begin = uint32_t(at);
		if (first == '[' || first == '{') {
			value.kind = first == '[' ? Value::Kind::Array : Value::Kind::Dictionary;
			items(value, first == '[' ? ']' : '}');
		} else if (word().empty()) {
			fail("expected a value");
		} else {
			// Array[int]( and Dictionary[String, int]( write their types right
			// after the name.
			if (peek() == '[') {
				Value types;
				items(types, ']');
			}
			while (peek() == ' ' || peek() == '\t') {
				at++;
			}
			if (peek() == '(') {
				value.kind = Value::Kind::Call;
				items(value, ')');
			}
		}
		value.end = uint32_t(at);
		return value;
	}

	// The sections of the file, each with its entries.
	std::vector<Section> sections() {
		std::vector<Section> found;
		while (true) {
			skip_space();
			if (at >= text.size()) {
				return found;
			}
			if (peek() == '[') {
				Section section;
				section.begin = uint32_t(at++);
				section.tag = word();
				while (true) {
					skip_space();
					if (peek() == ']') {
						at++;
						break;
					}
					const std::string field = word();
					skip_space();
					if (field.empty() || peek() != '=') {
						fail("expected a field of a section's header");
						return found;
					}
					at++;
					section.fields.emplace_back(field, value());
				}
				found.push_back(std::move(section));
				continue;
			}
			if (found.empty()) {
				fail("expected a section");
				return found;
			}
			Entry entry;
			entry.begin = uint32_t(at);
			if (peek() == '"') {
				const Value quoted = string();
				entry.key = decode(text, quoted);
				entry.end = uint32_t(at);
				skip_space();
			} else {
				// The engine reads a bare key up to `=`, leaving out whitespace.
				while (at < text.size() && text[at] != '=' && text[at] != '\n' && text[at] != '"' && text[at] != ';') {
					if (!is_space(text[at])) {
						entry.key += text[at];
						entry.end = uint32_t(at + 1);
					}
					at++;
				}
			}
			if (peek() != '=') {
				fail("expected `=` after a key");
				return found;
			}
			at++;
			entry.value = value();
			found.back().entries.push_back(std::move(entry));
		}
	}

	// A string's text as the engine reads a name in it, with the offset in
	// `p_text` where each byte of it begins, and its end. In the names and
	// node paths it writes, the engine escapes only quotes, apostrophes and
	// backslashes (core/string/ustring.cpp:4473-4494, 5052-5063; ed1daf0).
	// Another escape reads here as the character after the backslash, so a
	// name with one does not match the old name, which refuses.
	static std::string decode(const std::string &p_text, const Value &p_string, std::vector<uint32_t> *r_offsets = nullptr) {
		std::string decoded;
		for (uint32_t i = p_string.begin; i < p_string.end; i++) {
			if (r_offsets) {
				r_offsets->push_back(i);
			}
			i += p_text[i] == '\\' && i + 1 < p_string.end ? 1 : 0;
			decoded += p_text[i];
		}
		if (r_offsets) {
			r_offsets->push_back(p_string.end);
		}
		return decoded;
	}
};

std::string file_of(const std::string &p_resource) {
	return p_resource.substr(0, p_resource.find("::"));
}

// A sub-resource's id, or "" for the file's own resource.
std::string id_of(const std::string &p_resource) {
	const size_t at = p_resource.find("::");
	return at == std::string::npos ? std::string() : p_resource.substr(at + 2);
}

const Value *field_value(const Section &p_section, const std::string &p_name) {
	for (const auto &[name, value] : p_section.fields) {
		if (name == p_name) {
			return &value;
		}
	}
	return nullptr;
}

bool holds(const std::string &p_text, const Section &p_section, const StoredName &p_name) {
	// A string, or a bare token such as an id of format 2; "" for none.
	const auto field = [&](const char *p_field) {
		const Value *value = field_value(p_section, p_field);
		return !value ? std::string() : value->kind == Value::Kind::String ? Reader::decode(p_text, *value)
																		   : p_text.substr(value->begin, value->end - value->begin);
	};
	// The engine writes the ends of a connection simplified, as the scene
	// index reads them (scene/resources/resource_format_text.cpp:2107-2108;
	// ed1daf0).
	if (p_name.kind == StoredName::Kind::Connection) {
		const SceneConnection &connection = p_name.connection;
		return p_section.tag == "connection" && field("signal") == connection.signal && field("from") == connection.source && field("to") == connection.target && field("method") == connection.method;
	}
	if (p_name.node) {
		const std::string parent = field("parent");
		return p_section.tag == "node" && (parent.empty() ? std::string(".") : parent == "." ? field("name")
																							 : parent + "/" + field("name")) == p_name.section;
	}
	return p_name.section.empty() ? p_section.tag == "resource" : p_section.tag == "sub_resource" && field("id") == p_name.section;
}

// A typed container's contents, as Array[int]([…]).
const Value &unwrapped(const Value &p_value) {
	return p_value.kind == Value::Kind::Call && p_value.items.size() == 1 && p_value.items[0].kind != Value::Kind::Other ? p_value.items[0] : p_value;
}

// The value of the String key `p_key` of a Dictionary, or null.
const Value *entry_of(const std::string &p_text, const Value &p_dictionary, const std::string &p_key) {
	const Value &dictionary = unwrapped(p_dictionary);
	for (size_t i = 0; dictionary.kind == Value::Kind::Dictionary && i + 1 < dictionary.items.size(); i += 2) {
		const Value &key = dictionary.items[i];
		if (key.kind == Value::Kind::String && Reader::decode(p_text, key) == p_key) {
			return &dictionary.items[i + 1];
		}
	}
	return nullptr;
}

} // namespace

bool StoredName::operator<(const StoredName &p_other) const {
	const SceneConnection &a = connection;
	const SceneConnection &b = p_other.connection;
	return std::tie(kind, node, section, key, items, a.signal, a.source, a.target, a.method) < std::tie(p_other.kind, p_other.node, p_other.section, p_other.key, p_other.items, b.signal, b.source, b.target, b.method);
}

std::string StoredName::shown() const {
	const std::string owner = node ? (section == "." ? "the root" : "the node " + section) : section.empty() ? "the resource"
																											 : "the resource " + section;
	switch (kind) {
		case Kind::Key:
			return "\"" + key + "\" of " + owner + (items.empty() ? "" : " (in its value)");
		case Kind::Connection:
			return "the connection of \"" + connection.signal + "\" from " + connection.source + " to " + connection.target;
		case Kind::Path:
			return key + " of " + owner;
		case Kind::Method:
			return "key " + std::to_string(items[0]) + " of " + key + " of " + owner;
	}
	return std::string();
}

std::vector<std::pair<std::string, StoredName>> stored_names(const Reference &p_reference) {
	std::vector<std::pair<std::string, StoredName>> found;
	StoredName name;
	if (!p_reference.named.from.empty()) {
		const SceneNamedMember &named = p_reference.named;
		for (const ScenePlace &place : named.places) {
			const std::string index = std::to_string(place.index);
			name.kind = named.method ? StoredName::Kind::Method : StoredName::Kind::Path;
			name.section = id_of(place.resource);
			name.key = named.animation.empty() ? "properties/" + index + "/path" : "tracks/" + index + (named.method ? "/keys" : "/path");
			name.items = named.method ? std::vector<int>{ place.key } : std::vector<int>();
			found.emplace_back(file_of(place.resource), name);
		}
	} else if (!p_reference.connection.signal.empty()) {
		name.kind = StoredName::Kind::Connection;
		name.connection = p_reference.connection;
		found.emplace_back(p_reference.connection.file, name);
	} else {
		name.node = p_reference.resource.empty();
		name.section = name.node ? p_reference.node : id_of(p_reference.resource);
		name.key = p_reference.property;
		name.items = p_reference.value_path;
		found.emplace_back(p_reference.scene, name);
	}
	return found;
}

LocatedNames locate_names(const std::string &p_text, const std::set<StoredName> &p_names, bool p_signal, const std::string &p_old, const std::string &p_new) {
	using Kind = StoredName::Kind;
	LocatedNames located;
	const std::vector<uint32_t> starts = line_starts_of(p_text);
	const auto refuse = [&](std::optional<uint32_t> p_at, const std::string &p_error) {
		located.edits.clear();
		located.error = p_error;
		located.line = p_at ? position_in(p_text, starts, *p_at).line : 0;
		return located;
	};
	Reader reader(p_text);
	const std::vector<Section> sections = reader.sections();
	if (!reader.error.empty()) {
		return refuse(reader.error_at, "The file cannot be read: " + reader.error + ".");
	}
	// A key is written plain unless it needs quotes; in a string, a name needs
	// no escapes (core/string/ustring.cpp:4474-4496, 5053-5064; ed1daf0).
	const bool plain = std::none_of(p_new.begin(), p_new.end(), [](char p_char) { return static_cast<unsigned char>(p_char) > 126; });
	const auto edit = [&](const Value &p_string) {
		return CheckedEdit{ p_string.begin, p_string.end, p_text.substr(p_string.begin, p_string.end - p_string.begin), p_new };
	};
	for (const StoredName &name : p_names) {
		// Each place found, with the name it holds as the engine reads it.
		std::vector<std::pair<CheckedEdit, std::string>> found;
		std::string problem;
		uint32_t problem_at = 0;
		const auto wrong = [&](uint32_t p_at, const std::string &p_problem) {
			if (problem.empty()) {
				problem = p_problem;
				problem_at = p_at;
			}
		};
		const auto in_string = [&](const Value *p_value, uint32_t p_at) {
			if (!p_value || p_value->kind != Value::Kind::String) {
				wrong(p_at, name.shown() + " holds no name where the reference says.");
				return;
			}
			found.push_back({ edit(*p_value), Reader::decode(p_text, *p_value) });
		};
		for (const Section &section : sections) {
			if (!holds(p_text, section, name)) {
				continue;
			}
			if (name.kind == Kind::Connection) {
				in_string(field_value(section, p_signal ? "signal" : "method"), section.begin);
				continue;
			}
			for (const Entry &entry : section.entries) {
				if (entry.key != name.key) {
					continue;
				}
				if (name.kind == Kind::Key && name.items.empty()) {
					found.push_back({ { entry.begin, entry.end, p_text.substr(entry.begin, entry.end - entry.begin), plain ? p_new : "\"" + p_new + "\"" }, entry.key });
					for (const Entry &other : section.entries) {
						if (other.key == p_new) {
							wrong(other.begin, name.shown() + " would be renamed beside \"" + p_new + "\", which is stored already and would then take effect.");
						}
					}
				} else if (name.kind == Kind::Key) {
					const Value *value = &entry.value;
					const Value *container = nullptr;
					for (const int item : name.items) {
						container = value ? &unwrapped(*value) : nullptr;
						const bool holds_items = container && (container->kind == Value::Kind::Array || container->kind == Value::Kind::Dictionary);
						value = holds_items && item >= 0 && size_t(item) < container->items.size() ? &container->items[item] : nullptr;
					}
					in_string(value, entry.begin);
					// A struct's field renamed beside a key that has the new name.
					for (size_t i = 0; value && container->kind == Value::Kind::Dictionary && name.items.back() % 2 == 0 && i < container->items.size(); i += 2) {
						const Value &other = container->items[i];
						if (other.kind == Value::Kind::String && Reader::decode(p_text, other) == p_new) {
							wrong(other.begin, name.shown() + " would be renamed beside \"" + p_new + "\", which is stored already and would then take effect.");
						}
					}
				} else if (name.kind == Kind::Method) {
					const Value *values = entry_of(p_text, entry.value, "values");
					const Value *list = values ? &unwrapped(*values) : nullptr;
					const size_t key = size_t(name.items[0]);
					in_string(list && list->kind == Value::Kind::Array && key < list->items.size() ? entry_of(p_text, list->items[key], "method") : nullptr, entry.begin);
				} else {
					const Value &path = entry.value;
					const Value *string = path.kind == Value::Kind::Call && path.items.size() == 1 && path.items[0].kind == Value::Kind::String ? &path.items[0] : nullptr;
					std::vector<uint32_t> offsets;
					const std::string decoded = string ? Reader::decode(p_text, *string, &offsets) : std::string();
					// The first subname, after the first colon.
					const size_t colon = decoded.find(':');
					if (colon == std::string::npos) {
						wrong(entry.begin, name.shown() + " holds no property where the reference says.");
						continue;
					}
					const size_t end = std::min(decoded.find(':', colon + 1), decoded.size());
					const uint32_t begin = offsets[colon + 1];
					found.push_back({ { begin, offsets[end], p_text.substr(begin, offsets[end] - begin), p_new }, decoded.substr(colon + 1, end - colon - 1) });
				}
			}
		}
		if (!problem.empty()) {
			return refuse(problem_at, problem);
		}
		if (found.size() != 1) {
			return refuse(found.empty() ? std::nullopt : std::optional<uint32_t>(found[1].first.begin), name.shown() + (found.empty() ? " is not found." : " is found " + std::to_string(found.size()) + " times."));
		}
		if (found[0].second != p_old) {
			return refuse(found[0].first.begin, name.shown() + " names \"" + found[0].second + "\", not \"" + p_old + "\".");
		}
		located.edits.push_back(found[0].first);
	}
	std::sort(located.edits.begin(), located.edits.end(), [](const CheckedEdit &p_a, const CheckedEdit &p_b) { return p_a.begin < p_b.begin; });
	for (size_t i = 1; i < located.edits.size(); i++) {
		if (located.edits[i].begin < located.edits[i - 1].end) {
			return refuse(located.edits[i].begin, "Two references name the same place.");
		}
	}
	return located;
}

} // namespace petrichor::compiler
