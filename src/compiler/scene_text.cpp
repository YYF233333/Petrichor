#include "compiler/scene_text.h"

#include "compiler/lexer.h"

#include <algorithm>
#include <optional>
#include <set>
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

void append_utf8(std::string &r_text, uint32_t p_code) {
	if (p_code < 0x80) {
		r_text += char(p_code);
	} else if (p_code < 0x800) {
		r_text += char(0xC0 | (p_code >> 6));
		r_text += char(0x80 | (p_code & 0x3F));
	} else if (p_code < 0x10000) {
		r_text += char(0xE0 | (p_code >> 12));
		r_text += char(0x80 | ((p_code >> 6) & 0x3F));
		r_text += char(0x80 | (p_code & 0x3F));
	} else {
		r_text += char(0xF0 | (p_code >> 18));
		r_text += char(0x80 | ((p_code >> 12) & 0x3F));
		r_text += char(0x80 | ((p_code >> 6) & 0x3F));
		r_text += char(0x80 | (p_code & 0x3F));
	}
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
				entry.key = decode(text, quoted.begin, quoted.end);
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

	// A string's text as the engine reads it (variant_parser.cpp:277-415),
	// with the offset in `p_text` where each byte of it begins, and its end.
	static std::string decode(const std::string &p_text, uint32_t p_begin, uint32_t p_end, std::vector<uint32_t> *r_offsets = nullptr) {
		std::string decoded;
		const auto mark = [&](uint32_t p_at) {
			if (r_offsets) {
				r_offsets->resize(decoded.size(), p_at);
			}
		};
		uint32_t lead = 0;
		for (uint32_t i = p_begin; i < p_end;) {
			const uint32_t start = i;
			if (p_text[i] != '\\' || i + 1 >= p_end) {
				decoded += p_text[i++];
				mark(start);
				continue;
			}
			const char next = p_text[i + 1];
			i += 2;
			uint32_t code = 0;
			switch (next) {
				case 'b':
					code = 8;
					break;
				case 't':
					code = 9;
					break;
				case 'n':
					code = 10;
					break;
				case 'f':
					code = 12;
					break;
				case 'r':
					code = 13;
					break;
				case 'u':
				case 'U': {
					const uint32_t digits = next == 'U' ? 6 : 4;
					for (uint32_t d = 0; d < digits && i < p_end; d++, i++) {
						const char c = p_text[i];
						code = (code << 4) | uint32_t(c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10);
					}
				} break;
				default:
					code = static_cast<unsigned char>(next);
			}
			if ((code & 0xFFFFFC00) == 0xD800) {
				lead = code;
				continue;
			}
			if ((code & 0xFFFFFC00) == 0xDC00 && lead) {
				code = (lead << 10) + code - ((0xD800 << 10) + 0xDC00 - 0x10000);
			}
			lead = 0;
			append_utf8(decoded, code);
			mark(start);
		}
		if (r_offsets) {
			r_offsets->push_back(p_end);
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

// A node path from a scene's root: "." for the root, "Head/Camera".
std::string from_root(std::string p_path) {
	if (p_path.rfind("./", 0) == 0) {
		p_path = p_path.substr(2);
	}
	return p_path.empty() ? std::string(".") : p_path;
}

// A place in the file that stores a name.
struct Site {
	enum class Kind {
		Key, // A key of a node or a resource, or a String down its value.
		Connection, // The signal or the method of a connection.
		Path, // The first subname of a track's or a synchronized property's NodePath.
		Method, // The method of a key of a method track.
	};
	Kind kind = Kind::Key;
	bool node = false;
	std::string section; // A node's path, or a resource's id.
	std::string key;
	std::vector<int> items; // For a Key, down its value; for a Method, the key's index.
	SceneConnection connection;

	bool operator<(const Site &p_other) const {
		const SceneConnection &a = connection;
		const SceneConnection &b = p_other.connection;
		return std::tie(kind, node, section, key, items, a.signal, a.source, a.target, a.method) < std::tie(p_other.kind, p_other.node, p_other.section, p_other.key, p_other.items, b.signal, b.source, b.target, b.method);
	}

	std::string shown() const {
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
};

void add_sites(const Reference &p_reference, const std::string &p_file, std::set<Site> &r_sites) {
	Site site;
	if (!p_reference.named.from.empty()) {
		const SceneNamedMember &named = p_reference.named;
		for (const ScenePlace &place : named.places) {
			if (file_of(place.resource) != p_file) {
				continue;
			}
			site.section = id_of(place.resource);
			const std::string index = std::to_string(place.index);
			site.kind = named.method ? Site::Kind::Method : Site::Kind::Path;
			site.key = named.animation.empty() ? "properties/" + index + "/path" : "tracks/" + index + (named.method ? "/keys" : "/path");
			site.items = named.method ? std::vector<int>{ place.key } : std::vector<int>();
			r_sites.insert(site);
		}
	} else if (!p_reference.connection.signal.empty()) {
		if (p_reference.connection.file == p_file) {
			site.kind = Site::Kind::Connection;
			site.connection = p_reference.connection;
			site.connection.binds = 0;
			site.connection.unbinds = 0;
			r_sites.insert(site);
		}
	} else if (p_reference.scene == p_file) {
		site.node = p_reference.resource.empty();
		site.section = site.node ? p_reference.node : id_of(p_reference.resource);
		site.key = p_reference.property;
		site.items = p_reference.value_path;
		r_sites.insert(site);
	}
}

// The text of a field of a section's header: a string's, or a bare token's.
std::optional<std::string> field_text(const std::string &p_text, const Section &p_section, const std::string &p_name) {
	for (const auto &[name, value] : p_section.fields) {
		if (name == p_name) {
			return value.kind == Value::Kind::String ? Reader::decode(p_text, value.begin, value.end) : p_text.substr(value.begin, value.end - value.begin);
		}
	}
	return std::nullopt;
}

const Value *field_value(const Section &p_section, const std::string &p_name) {
	for (const auto &[name, value] : p_section.fields) {
		if (name == p_name) {
			return &value;
		}
	}
	return nullptr;
}

bool holds(const std::string &p_text, const Section &p_section, const Site &p_site) {
	if (p_site.kind == Site::Kind::Connection) {
		const SceneConnection &connection = p_site.connection;
		return p_section.tag == "connection" && field_text(p_text, p_section, "signal") == connection.signal && from_root(field_text(p_text, p_section, "from").value_or("")) == connection.source &&
				from_root(field_text(p_text, p_section, "to").value_or("")) == connection.target && field_text(p_text, p_section, "method") == connection.method;
	}
	if (p_site.node) {
		if (p_section.tag != "node") {
			return false;
		}
		const std::optional<std::string> parent = field_text(p_text, p_section, "parent");
		const std::string name = field_text(p_text, p_section, "name").value_or("");
		return (!parent ? std::string(".") : *parent == "." ? name
															: *parent + "/" + name) == p_site.section;
	}
	return p_site.section.empty() ? p_section.tag == "resource" : p_section.tag == "sub_resource" && field_text(p_text, p_section, "id") == p_site.section;
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
		if (key.kind == Value::Kind::String && Reader::decode(p_text, key.begin, key.end) == p_key) {
			return &dictionary.items[i + 1];
		}
	}
	return nullptr;
}

} // namespace

std::vector<std::string> files_storing(const Reference &p_reference) {
	if (!p_reference.named.from.empty()) {
		std::set<std::string> files;
		for (const ScenePlace &place : p_reference.named.places) {
			files.insert(file_of(place.resource));
		}
		return std::vector<std::string>(files.begin(), files.end());
	}
	return { p_reference.connection.signal.empty() ? p_reference.scene : p_reference.connection.file };
}

LocatedNames locate_names(const std::string &p_text, const std::string &p_file, const std::vector<Reference> &p_references, bool p_signal, const std::string &p_old, const std::string &p_new) {
	LocatedNames located;
	const std::vector<uint32_t> starts = line_starts_of(p_text);
	const auto refuse = [&](uint32_t p_at, const std::string &p_error) {
		located.edits.clear();
		located.error = p_error;
		located.line = position_in(p_text, starts, p_at).line;
		return located;
	};
	Reader reader(p_text);
	const std::vector<Section> sections = reader.sections();
	if (!reader.error.empty()) {
		return refuse(reader.error_at, "The file cannot be read: " + reader.error + ".");
	}
	std::set<Site> sites;
	for (const Reference &reference : p_references) {
		add_sites(reference, p_file, sites);
	}
	// A key is written plain unless it needs quotes; in a string, a name needs
	// no escapes (core/string/ustring.cpp:4474-4496, 5053-5064; ed1daf0).
	const bool plain = std::none_of(p_new.begin(), p_new.end(), [](char p_char) { return static_cast<unsigned char>(p_char) > 126; });
	for (const Site &site : sites) {
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
				wrong(p_at, site.shown() + " holds no name where the reference says.");
				return;
			}
			found.push_back({ { p_value->begin, p_value->end, p_text.substr(p_value->begin, p_value->end - p_value->begin), p_new }, Reader::decode(p_text, p_value->begin, p_value->end) });
		};
		// Another key `p_new` beside the one renamed.
		const auto beside = [&](uint32_t p_at) {
			wrong(p_at, site.shown() + " would be renamed beside \"" + p_new + "\", which is stored already and would then take effect.");
		};
		for (const Section &section : sections) {
			if (!holds(p_text, section, site)) {
				continue;
			}
			if (site.kind == Site::Kind::Connection) {
				in_string(field_value(section, p_signal ? "signal" : "method"), section.begin);
				continue;
			}
			for (const Entry &entry : section.entries) {
				if (entry.key != site.key) {
					continue;
				}
				if (site.kind == Site::Kind::Key && site.items.empty()) {
					found.push_back({ { entry.begin, entry.end, p_text.substr(entry.begin, entry.end - entry.begin), plain ? p_new : "\"" + p_new + "\"" }, entry.key });
					for (const Entry &other : section.entries) {
						if (other.key == p_new) {
							beside(other.begin);
						}
					}
				} else if (site.kind == Site::Kind::Key) {
					const Value *value = &entry.value;
					const Value *container = nullptr;
					for (const int item : site.items) {
						container = value ? &unwrapped(*value) : nullptr;
						const bool holds_items = container && (container->kind == Value::Kind::Array || container->kind == Value::Kind::Dictionary);
						value = holds_items && item >= 0 && size_t(item) < container->items.size() ? &container->items[item] : nullptr;
					}
					in_string(value, entry.begin);
					// A struct's field renamed beside a key that has the new name.
					for (size_t i = 0; value && container->kind == Value::Kind::Dictionary && site.items.back() % 2 == 0 && i < container->items.size(); i += 2) {
						const Value &other = container->items[i];
						if (other.kind == Value::Kind::String && Reader::decode(p_text, other.begin, other.end) == p_new) {
							beside(other.begin);
						}
					}
				} else if (site.kind == Site::Kind::Method) {
					const Value *values = entry_of(p_text, entry.value, "values");
					const Value *list = values ? &unwrapped(*values) : nullptr;
					const size_t key = size_t(site.items[0]);
					in_string(list && list->kind == Value::Kind::Array && key < list->items.size() ? entry_of(p_text, list->items[key], "method") : nullptr, entry.begin);
				} else {
					const Value &path = entry.value;
					const Value *string = path.kind == Value::Kind::Call && path.items.size() == 1 ? &path.items[0] : nullptr;
					std::vector<uint32_t> offsets;
					const std::string decoded = string && string->kind == Value::Kind::String ? Reader::decode(p_text, string->begin, string->end, &offsets) : std::string();
					// The first subname, after the first colon.
					const size_t colon = decoded.find(':');
					if (colon == std::string::npos) {
						wrong(entry.begin, site.shown() + " holds no property where the reference says.");
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
			return refuse(found.empty() ? 0 : found[1].first.begin, site.shown() + (found.empty() ? " is not found." : " is found " + std::to_string(found.size()) + " times."));
		}
		if (found[0].second != p_old) {
			return refuse(found[0].first.begin, site.shown() + " names \"" + found[0].second + "\", not \"" + p_old + "\".");
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
