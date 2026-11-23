#include "adapter/editor/lsp_server.h"

#include "adapter/editor/scene_index.h"
#include "adapter/editor/script_compiler.h"
#include "adapter/pet_compiled.h"
#include "build_id.gen.h"
#include "compiler/analysis.h"
#include "compiler/formatter.h"
#include "compiler/lexer.h"
#include "runtime/text.h"

#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/editor_debugger_session.hpp>
#include <godot_cpp/classes/editor_file_system.hpp>
#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/editor_settings.hpp>
#include <godot_cpp/classes/editor_toaster.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/json.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/classes/script.hpp>
#include <godot_cpp/classes/script_editor.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstring>
#include <functional>
#include <iterator>
#include <optional>
#include <set>
#include <tuple>

using namespace godot;

namespace petrichor {

namespace {

constexpr size_t MAX_MESSAGE = size_t(16) << 20;
constexpr size_t MAX_HEADER = size_t(64) << 10;
constexpr size_t MAX_UNSENT = size_t(128) << 20; // For a client that reads nothing.
constexpr int32_t CHUNK = 4 << 20; // Read or sent in a frame.
constexpr uint64_t FRAME_BUDGET = 50; // In ms, for the messages and diagnostics of a frame.
// In ms. A client sends `initialize` as it connects; a connection that never
// does is let go.
constexpr uint64_t INITIALIZE_WAIT = 10000;

#if defined(_WIN32) || defined(__APPLE__)
constexpr bool FOLDS_CASE = true; // As their file systems do by default.
#else
constexpr bool FOLDS_CASE = false;
#endif

// JSON-RPC 2.0 and LSP 3.17.
constexpr int PARSE_ERROR = -32700;
constexpr int INVALID_REQUEST = -32600;
constexpr int METHOD_NOT_FOUND = -32601;
constexpr int INVALID_PARAMS = -32602;
constexpr int SERVER_NOT_INITIALIZED = -32002;
constexpr int REQUEST_FAILED = -32803;

String info_path() {
	return ProjectSettings::get_singleton()->globalize_path("res://.godot/petrichor/lsp.json");
}

char lower_char(char p_char) {
	return p_char >= 'A' && p_char <= 'Z' ? char(p_char - 'A' + 'a') : p_char;
}

std::string lower_ascii(std::string p_text) {
	std::transform(p_text.begin(), p_text.end(), p_text.begin(), lower_char);
	return p_text;
}

// Strict, so that no fault reaches the engine's String, which prints one.
bool is_utf8(const std::string &p_text) {
	for (size_t at = 0; at < p_text.size();) {
		const unsigned char lead = p_text[at];
		size_t length = 1;
		uint32_t code = lead;
		uint32_t least = 0;
		if ((lead & 0xE0) == 0xC0) {
			length = 2;
			code = lead & 0x1F;
			least = 0x80;
		} else if ((lead & 0xF0) == 0xE0) {
			length = 3;
			code = lead & 0x0F;
			least = 0x800;
		} else if ((lead & 0xF8) == 0xF0) {
			length = 4;
			code = lead & 0x07;
			least = 0x10000;
		} else if (lead >= 0x80) {
			return false;
		}
		if (at + length > p_text.size()) {
			return false;
		}
		for (size_t i = 1; i < length; i++) {
			const unsigned char next = p_text[at + i];
			if ((next & 0xC0) != 0x80) {
				return false;
			}
			code = code << 6 | (next & 0x3F);
		}
		if (code < least || code > 0x10FFFF || (code >= 0xD800 && code <= 0xDFFF)) {
			return false;
		}
		at += length;
	}
	return true;
}

std::string trimmed(const std::string &p_text) {
	const size_t begin = p_text.find_first_not_of(" \t");
	return begin == std::string::npos ? std::string() : p_text.substr(begin, p_text.find_last_not_of(" \t") - begin + 1);
}

// The value of a header's Content-Length line, the last one; nullopt without
// one that is a number.
std::optional<size_t> content_length(const std::string &p_header) {
	std::optional<size_t> length;
	for (size_t begin = 0; begin <= p_header.size();) {
		const size_t end = std::min(p_header.find("\r\n", begin), p_header.size());
		const std::string line = p_header.substr(begin, end - begin);
		begin = end + 2;
		const size_t colon = line.find(':');
		if (colon == std::string::npos || lower_ascii(trimmed(line.substr(0, colon))) != "content-length") {
			continue;
		}
		const std::string value = trimmed(line.substr(colon + 1));
		if (value.empty() || value.size() > 12 || value.find_first_not_of("0123456789") != std::string::npos) {
			return std::nullopt;
		}
		length = 0;
		for (const char digit : value) {
			*length = *length * 10 + size_t(digit - '0');
		}
	}
	return length;
}

// The JSON text of a message's body. A NUL escape becomes U+FFFD, of the same
// length in UTF-16, as the engine's String prints an error for each NUL
// (core/io/json.cpp:279-368, core/string/ustring.cpp:202-206, 313-316;
// ed1daf0). A NUL byte is no JSON, and the String would end at it (:1782).
std::optional<String> json_text(std::string p_body) {
	if (!is_utf8(p_body) || p_body.find('\0') != std::string::npos) {
		return std::nullopt;
	}
	// A backslash escapes the character after it, so the search goes on after that.
	for (size_t at = p_body.find('\\'); at != std::string::npos; at = p_body.find('\\', at + 2)) {
		if (p_body.compare(at + 1, 5, "u0000") == 0) {
			p_body.replace(at + 1, 5, "ufffd");
		}
	}
	return to_godot(p_body);
}

void send(std::string &r_out, Dictionary p_message) {
	p_message["jsonrpc"] = "2.0";
	const std::string body = to_utf8(JSON::stringify(p_message, "", false));
	r_out += "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
}

void reply(std::string &r_out, const Variant &p_id, const Variant &p_result) {
	Dictionary message;
	message["id"] = p_id;
	message["result"] = p_result;
	send(r_out, message);
}

void reply_error(std::string &r_out, const Variant &p_id, int p_code, const String &p_message) {
	Dictionary error;
	error["code"] = p_code;
	error["message"] = p_message;
	Dictionary message;
	message["id"] = p_id;
	message["error"] = error;
	send(r_out, message);
}

Dictionary dictionary(const Variant &p_value) {
	return p_value.get_type() == Variant::DICTIONARY ? Dictionary(p_value) : Dictionary();
}

// A request's id as it was written, a JSON number being a double; nil for one
// that is neither an integer nor a string.
Variant request_id(const Variant &p_id) {
	if (p_id.get_type() == Variant::STRING) {
		return p_id;
	}
	if (p_id.get_type() == Variant::FLOAT) {
		const double value = p_id;
		if (std::abs(value) <= 9007199254740992.0 && value == std::floor(value)) {
			return int64_t(value);
		}
	}
	return Variant();
}

// LSP 3.17, DiagnosticSeverity.
int severity(compiler::Severity p_severity) {
	switch (p_severity) {
		case compiler::Severity::Error:
			return 1;
		case compiler::Severity::Warning:
			return 2;
		case compiler::Severity::Hint:
			break;
	}
	return 4;
}

// A number, clamped to the range.
std::optional<int64_t> integer(const Variant &p_value, int64_t p_min, int64_t p_max) {
	if (p_value.get_type() != Variant::FLOAT || std::isnan(double(p_value))) {
		return std::nullopt;
	}
	return int64_t(std::clamp(double(p_value), double(p_min), double(p_max)));
}

std::optional<compiler::Utf16Position> position(const Variant &p_value) {
	const Dictionary place = dictionary(p_value);
	const std::optional<int64_t> line = integer(place.get("line", Variant()), 0, INT32_MAX);
	const std::optional<int64_t> character = integer(place.get("character", Variant()), 0, INT32_MAX);
	if (!line || !character) {
		return std::nullopt;
	}
	return compiler::Utf16Position{ uint32_t(*line), uint32_t(*character) };
}

Dictionary range(const compiler::Utf16Position &p_start, const compiler::Utf16Position &p_end) {
	const auto place = [](const compiler::Utf16Position &p_position) {
		Dictionary result;
		result["line"] = int64_t(p_position.line);
		result["character"] = int64_t(p_position.character);
		return result;
	};
	Dictionary result;
	result["start"] = place(p_start);
	result["end"] = place(p_end);
	return result;
}

int hex_digit(const std::string &p_text, size_t p_at) {
	const char c = p_at < p_text.size() ? p_text[p_at] : '\0';
	if (c >= '0' && c <= '9') {
		return c - '0';
	}
	if ((c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')) {
		return (c | 0x20) - 'a' + 10;
	}
	return -1;
}

#ifdef _WIN32
extern "C" __declspec(dllimport) unsigned long __stdcall GetLongPathNameW(const wchar_t *p_short, wchar_t *r_long, unsigned long p_size);
#endif

// The absolute path with each of Windows' 8.3 names in it as its long name,
// as far as the path exists; the rest, such as a file not saved yet, is kept.
// Clients and the engine may each have either name.
std::string long_path(const std::string &p_path) {
#ifdef _WIN32
	// An 8.3 name has a "~"; "C:/" ends the search.
	for (size_t end = p_path.size(); end != std::string::npos && end > 3 && p_path.find('~') < end; end = p_path.rfind('/', end - 1)) {
		const Char16String head = to_godot(p_path.substr(0, end)).replace("/", "\\").utf16();
		const wchar_t *from = reinterpret_cast<const wchar_t *>(head.get_data());
		std::u16string named(GetLongPathNameW(from, nullptr, 0), u'\0');
		const unsigned long size = named.empty() ? 0 : GetLongPathNameW(from, reinterpret_cast<wchar_t *>(named.data()), (unsigned long)named.size());
		if (size > 0 && size < named.size()) {
			return to_utf8(String::utf16(named.data(), int64_t(size)).replace("\\", "/")) + p_path.substr(end);
		}
	}
#endif
	return p_path;
}

// The project's root with long names, and a "/" at its end.
const std::string &project_root() {
	static const std::string root = [] {
		const std::string path = project_directory();
		return long_path(path.substr(0, path.size() - 1)) + "/";
	}();
	return root;
}

// The path with each name in it as the file system names it, where it has
// one in another case. The engine takes a path in another case for another
// file, and records it so (editor/file_system/editor_file_system.cpp:1964-1975,
// 2496-2503; ed1daf0).
std::string as_named(const std::string &p_relative) {
	String path = "res://";
	const PackedStringArray names = to_godot(p_relative).split("/");
	for (int64_t i = 0; i < names.size(); i++) {
		String name = names[i];
		PackedStringArray there;
		if (DirAccess::dir_exists_absolute(path)) {
			there = DirAccess::get_directories_at(path);
			there.append_array(DirAccess::get_files_at(path));
		}
		for (int64_t j = 0; !there.has(name) && j < there.size(); j++) {
			if (there[j].nocasecmp_to(name) == 0) {
				name = there[j];
			}
		}
		path = path.path_join(name);
	}
	return to_utf8(path);
}

// The res:// path of a file: URI under the project's root and outside
// .godot/; empty for any other. Windows clients write drive letters in either
// case and percent-encode the colon; there and on macOS, names may be in
// another case than the files'.
std::string project_path(const Variant &p_uri) {
	const std::string uri = p_uri.get_type() == Variant::STRING ? to_utf8(p_uri) : std::string();
	if (lower_ascii(uri.substr(0, 5)) != "file:") {
		return std::string();
	}
	std::string rest = uri.substr(5, uri.find_first_of("?#") - 5);
	if (rest.compare(0, 2, "//") == 0) {
		const size_t slash = rest.find('/', 2);
		const std::string host = lower_ascii(rest.substr(2, slash == std::string::npos ? std::string::npos : slash - 2));
		if (slash == std::string::npos || !(host.empty() || host == "localhost")) {
			return std::string();
		}
		rest.erase(0, slash);
	}
	std::string path;
	for (size_t at = 0; at < rest.size(); at++) {
		if (rest[at] != '%') {
			path += rest[at];
			continue;
		}
		const int high = hex_digit(rest, at + 1);
		const int low = hex_digit(rest, at + 2);
		if (high < 0 || low < 0) {
			return std::string();
		}
		path += char(high * 16 + low);
		at += 2;
	}
	if (!is_utf8(path) || std::any_of(path.begin(), path.end(), [](char c) { return static_cast<unsigned char>(c) < 0x20; })) {
		return std::string();
	}
	std::replace(path.begin(), path.end(), '\\', '/');
	if (path.size() > 2 && path[0] == '/' && path[2] == ':' && ((path[1] | 0x20) >= 'a' && (path[1] | 0x20) <= 'z')) {
		path.erase(0, 1);
	}
	path = long_path(path);
	const std::string &root = project_root();
	const auto folded = [](const std::string &p_text) { return FOLDS_CASE ? lower_ascii(p_text) : p_text; };
	if (path.size() <= root.size() || folded(path.substr(0, root.size())) != folded(root)) {
		return std::string();
	}
	const std::string relative = path.substr(root.size());
	for (size_t begin = 0; begin <= relative.size();) {
		const size_t end = std::min(relative.find('/', begin), relative.size());
		const std::string segment = relative.substr(begin, end - begin);
		if (segment.empty() || segment == "." || segment == ".." || (begin == 0 && folded(segment) == ".godot")) {
			return std::string();
		}
		begin = end + 1;
	}
	return FOLDS_CASE ? as_named(relative) : "res://" + relative;
}

// Of a script of this language; empty for other files.
std::string script_path(const Variant &p_uri) {
	const std::string path = project_path(p_uri);
	return path.size() > 4 && path.compare(path.size() - 4, 4, ".pet") == 0 ? path : std::string();
}

// A file the engine reads as text, which the engine's String takes without
// printing a fault: a script, or a scene or a resource file in the text format.
bool is_text(const std::string &p_path) {
	const std::string extension = lower_ascii(p_path.substr(p_path.find_last_of("./") + 1));
	return p_path.find("::") == std::string::npos && (extension == "pet" || extension == "gd" || extension == "cs" || extension == "tscn" || extension == "tres");
}

// The script a request is for, as the session sees it, and the offset of the
// request's position, which `p_position` requires.
struct Place {
	std::string path;
	std::string text;
	uint32_t offset = 0;
};

std::optional<Place> place_of(const compiler::ProjectApi &p_view, const Dictionary &p_params, bool p_position) {
	const std::string path = script_path(dictionary(p_params.get("textDocument", Variant())).get("uri", Variant()));
	const std::optional<std::string> text = path.empty() ? std::nullopt : p_view.script_source(path);
	const std::optional<compiler::Utf16Position> at = position(p_params.get("position", Variant()));
	if (!text || (p_position && !at)) {
		return std::nullopt;
	}
	return Place{ path, *text, at ? compiler::utf16_offset(*text, *at) : 0 };
}

uint32_t utf16_length(const std::string &p_line) {
	return compiler::utf16_positions(p_line, { uint32_t(p_line.size()) })[0].character;
}

Dictionary range_of(const std::string &p_text, uint32_t p_begin, uint32_t p_end) {
	const std::vector<compiler::Utf16Position> at = compiler::utf16_positions(p_text, { p_begin, std::max(p_begin, p_end) });
	return range(at[0], at[1]);
}

Dictionary text_edit(const std::string &p_text, uint32_t p_begin, uint32_t p_end, const std::string &p_new) {
	Dictionary edit;
	edit["range"] = range_of(p_text, p_begin, p_end);
	edit["newText"] = to_godot(p_new);
	return edit;
}

Dictionary diagnostic(const compiler::Diagnostic &p_diagnostic, const Dictionary &p_range) {
	Dictionary each;
	each["range"] = p_range;
	each["severity"] = severity(p_diagnostic.severity);
	each["source"] = "petrichor";
	each["message"] = to_godot(p_diagnostic.message);
	if (!p_diagnostic.warning.empty()) {
		each["code"] = to_godot(p_diagnostic.warning);
	}
	return each;
}

// Where the text that completion replaces begins, as the script editor finds
// it (scene/gui/code_edit.cpp:3837-3875; ed1daf0): in a string begun on the
// line, at its quote, which `r_quote` gets; otherwise at the word before the
// cursor, "/" taken as part of it.
uint32_t completion_start(const std::string &p_text, uint32_t p_offset, std::string &r_quote) {
	const size_t newline = p_offset == 0 ? std::string::npos : p_text.find_last_of("\r\n", p_offset - 1);
	const size_t line = newline == std::string::npos ? 0 : newline + 1;
	size_t opened = 0;
	r_quote.clear();
	for (size_t at = line; at < p_offset; at++) {
		const char c = p_text[at];
		if (!r_quote.empty()) {
			if (c == '\\') {
				at++;
			} else if (p_text.compare(at, r_quote.size(), r_quote) == 0) {
				at += r_quote.size() - 1;
				r_quote.clear();
			}
		} else if (c == '#') {
			break;
		} else if (c == '"' || c == '\'') {
			r_quote = std::string(p_text.compare(at, 3, std::string(3, c)) == 0 ? 3 : 1, c);
			opened = at;
			at += r_quote.size() - 1;
		}
	}
	if (!r_quote.empty()) {
		return uint32_t(opened);
	}
	const auto in_word = [](unsigned char p_char) { return p_char == '/' || p_char == '_' || std::isalnum(p_char) || p_char >= 0x7F; };
	size_t start = p_offset;
	while (start > line && in_word(static_cast<unsigned char>(p_text[start - 1]))) {
		start--;
	}
	return uint32_t(start);
}

String names_of(const PackedStringArray &p_files) {
	PackedStringArray names;
	for (const String &file : p_files) {
		names.push_back(file.get_file());
	}
	return String(", ").join(names);
}

// Why renaming in the script at `p_path` is refused, a line each, with the
// file and the line where there are any, as the editor lists them
// (PetReferences::set_refusals()).
String refused_rename(const std::vector<compiler::Diagnostic> &p_refusals, const std::string &p_path) {
	std::string text;
	for (const compiler::Diagnostic &each : p_refusals) {
		std::string place;
		if (each.line > 0) {
			place = each.file + ":" + std::to_string(each.line) + ": ";
		} else if (each.file != p_path) {
			place = each.file + ": ";
		}
		text += (text.empty() ? "" : "\n") + place + each.message;
	}
	return to_godot(text);
}

// LSP 3.17, CompletionItemKind, by compiler::CompletionKind.
constexpr int COMPLETION_KINDS[] = { 7, 2, 23, 6, 10, 13, 21, 18, 17, 1, 14 };
// LSP 3.17, SymbolKind, by compiler::Symbol::Kind.
constexpr int SYMBOL_KINDS[] = { 5, 11, 23, 10, 22, 14, 8, 6, 24 };
constexpr const char *CURSOR = "\xEF\xBF\xBF"; // U+FFFF, as the script editor marks the cursor.

// The project's files that Godot holds unsaved changes of: the script
// editor's tabs, and the open scenes (editor/script/script_editor_plugin.cpp:2408-2418,
// editor/editor_interface.cpp:738-748; ed1daf0).
std::set<std::string> godot_unsaved() {
	EditorInterface *editor = EditorInterface::get_singleton();
	std::set<std::string> files;
	for (const PackedStringArray &paths : { editor->get_script_editor()->get_unsaved_files(), editor->get_unsaved_scenes() }) {
		for (const String &path : paths) {
			if (path.begins_with("res://") && !path.contains("::")) {
				files.insert(to_utf8(path));
			}
		}
	}
	return files;
}

// For the title of a quick fix refused for the files held.
String held_note(const Held &p_held) {
	PackedStringArray godot;
	PackedStringArray elsewhere;
	for (const auto &[file, why] : p_held.files) {
		(why == Held::Elsewhere ? elsewhere : godot).push_back(to_godot(file));
	}
	PackedStringArray told;
	if (!godot.is_empty()) {
		told.push_back(vformat("save %s in Godot first", names_of(godot)));
	}
	if (!elsewhere.is_empty()) {
		told.push_back(vformat("save %s in the other editor first", names_of(elsewhere)));
	}
	return String("; ").join(told);
}

} // namespace

LspServer::LspServer(Ref<EditorDebuggerPlugin> p_debugger, std::function<void()> p_changed, std::function<void(const std::set<std::string> &)> p_written, Run p_run) :
		debugger(std::move(p_debugger)), editor_changed(std::move(p_changed)), editor_written(std::move(p_written)), run(std::move(p_run)) {
	delay = uint64_t(double(EditorInterface::get_singleton()->get_editor_settings()->get_setting("text_editor/completion/idle_parse_delay")) * 1000);
	server.instantiate();
	if (server->listen(0, "127.0.0.1") != OK) {
		UtilityFunctions::push_error("Petrichor: the language server cannot listen on 127.0.0.1.");
		return;
	}
	const String path = info_path();
	DirAccess::make_dir_recursive_absolute(path.get_base_dir());
	Dictionary info;
	info["pid"] = OS::get_singleton()->get_process_id();
	info["port"] = server->get_local_port();
	info["build"] = PETRICHOR_BUILD_ID;
	const Ref<FileAccess> file = FileAccess::open(path, FileAccess::WRITE);
	if (file.is_valid()) {
		file->store_string(JSON::stringify(info));
	}
	walked = walk_project(root);
	walker = std::thread(&LspServer::watch, this, root);
}

// The file stays if another editor has written it since.
LspServer::~LspServer() {
	if (walker.joinable()) {
		{
			std::lock_guard<std::mutex> lock(walking);
			stopping = true;
		}
		wake.notify_all();
		walker.join();
	}
	for (Session &each : sessions) {
		each.connection.peer->disconnect_from_host();
	}
	server->stop();
	const String path = info_path();
	Ref<JSON> json;
	json.instantiate();
	if (FileAccess::file_exists(path) && json->parse(FileAccess::get_file_as_string(path)) == OK && integer(dictionary(json->get_data()).get("pid", Variant()), 0, INT32_MAX) == OS::get_singleton()->get_process_id()) {
		DirAccess::remove_absolute(path);
	}
}

// Sends what is due, as much as the socket takes; false once sending fails.
bool LspServer::flush(Connection &r_connection) {
	if (r_connection.out.empty()) {
		return true;
	}
	const size_t size = std::min(r_connection.out.size(), size_t(CHUNK));
	PackedByteArray bytes;
	bytes.resize(int64_t(size));
	std::memcpy(bytes.ptrw(), r_connection.out.data(), size);
	const Array sent = r_connection.peer->put_partial_data(bytes);
	if (int64_t(sent[0]) != OK) {
		return false;
	}
	r_connection.out.erase(0, size_t(int64_t(sent[1])));
	return true;
}

// Sends what is due and reads what arrived; false once the connection is gone,
// or is closing and has sent everything.
bool LspServer::transfer(Connection &r_connection) {
	r_connection.peer->poll();
	if (r_connection.peer->get_status() != StreamPeerSocket::STATUS_CONNECTED || !flush(r_connection)) {
		return false;
	}
	if (r_connection.out.size() > MAX_UNSENT || (r_connection.closing && r_connection.out.empty())) {
		return false;
	}
	const int32_t available = r_connection.peer->get_available_bytes();
	if (available > 0 && !r_connection.closing) {
		const Array got = r_connection.peer->get_partial_data(std::min(available, CHUNK));
		if (int64_t(got[0]) != OK) {
			return false;
		}
		const PackedByteArray bytes = got[1];
		r_connection.in.append(reinterpret_cast<const char *>(bytes.ptr()), size_t(bytes.size()));
	}
	return true;
}

// Not while a pass of the analysis reads the workspace, nor within itself, as
// when what it calls draws a progress dialog, which runs a frame
// (editor/gui/progress_dialog.cpp:148-154, 227, 248; ed1daf0).
void LspServer::poll() {
	if (polling || !server->is_listening() || workspace().in_pass()) {
		return;
	}
	polling = true;
	step();
	// What the frame computed goes out in it, not a frame later; a failure is
	// seen as the next frame reads.
	for (Session &each : sessions) {
		flush(each.connection);
	}
	polling = false;
}

// Both only go up, so their sum changes with either.
uint64_t LspServer::revision() const {
	return workspace().revision() + edited;
}

void LspServer::step() {
	Time *time = Time::get_singleton();
	const uint64_t start = time->get_ticks_msec();
	for (auto each = sessions.begin(); each != sessions.end();) {
		const bool gone = !transfer(each->connection) || (each->state == State::Waiting && start - each->connection.since > INITIALIZE_WAIT);
		each = gone ? end(each) : std::next(each);
	}
	while (server->is_connection_available()) {
		Connection &connection = sessions.emplace_back().connection;
		connection.peer = server->take_connection();
		connection.since = start;
	}
	// A message of each session at least, so that none waits for another's.
	for (Session &each : sessions) {
		session = &each;
		Connection &connection = each.connection;
		for (bool first = true; !connection.closing && (first || time->get_ticks_msec() - start < FRAME_BUDGET); first = false) {
			size_t skipped = 0;
			const std::optional<std::string> body = connection.next(skipped);
			if (skipped > 0 && each.state == State::Running) {
				show_message(vformat("Petrichor's language server skipped a message of %d bytes, as it reads messages of up to %d.", int64_t(skipped), int64_t(MAX_MESSAGE)));
			}
			if (!body) {
				break;
			}
			handle(*body);
		}
		connection.in.erase(0, connection.read);
		connection.read = 0;
	}
	session = nullptr;
	// After the sessions' messages, so that a client's text that came with its
	// write is in place before the write is read back.
	if (stale) {
		refresh();
	}
	if (!conflicts.empty()) {
		held();
	}
	const auto serving = [](const Session &p_session) { return p_session.state == State::Running && !p_session.connection.closing; };
	if (std::none_of(sessions.begin(), sessions.end(), serving)) {
		return;
	}
	// Once the workspace and the documents have stayed for as long as the
	// script editor waits before it checks a script, or the editor has taken
	// in a change; a script just opened at once (architecture §2.4). One a
	// frame at least, more within the frame's budget, the rest the frame after.
	const uint64_t now = revision();
	if (now != seen) {
		seen = now;
		quiet_since = start;
	}
	const bool quiet = settle || start - quiet_since >= delay || continuing == now;
	settle = false;
	continuing = 0;
	// The sessions' changes count as the editor's own, which the editor checks
	// once a revision. The settle it then asks for is this one.
	if (quiet && told != workspace().revision()) {
		told = workspace().revision();
		editor_changed();
		settle = false;
	}
	bool sent = false;
	for (Session &each : sessions) {
		for (auto &[path, document] : each.documents) {
			if (!serving(each) || document.published == now || !(quiet || document.published == 0)) {
				continue;
			}
			if (sent && time->get_ticks_msec() - start >= FRAME_BUDGET) {
				continuing = quiet ? now : 0;
				return;
			}
			session = &each;
			publish(path, document);
			session = nullptr;
			sent = true;
		}
	}
}

std::optional<std::string> LspServer::Connection::next(size_t &r_skipped) {
	while (true) {
		const size_t dropped = std::min(skip, in.size() - read);
		read += dropped;
		skip -= dropped;
		if (skip > 0) {
			return std::nullopt;
		}
		// The body of a message without a usable length comes first, so the
		// next header is taken to begin at the next Content-Length, in any case.
		if (seeking) {
			const char *name = "content-length:";
			const auto found = std::search(in.begin() + int64_t(read), in.end(), name, name + 15, [](char p_in, char p_name) { return lower_char(p_in) == p_name; });
			if (found == in.end()) {
				read = std::max(read, in.size() - std::min(in.size(), size_t(14)));
				return std::nullopt;
			}
			read = size_t(found - in.begin());
			seeking = false;
		}
		const size_t end = in.find("\r\n\r\n", read);
		if (end == std::string::npos) {
			if (in.size() - read > MAX_HEADER) {
				read = in.size() - 3;
			}
			return std::nullopt;
		}
		const std::optional<size_t> length = content_length(in.substr(read, end - read));
		const size_t body = end + 4;
		if (length && *length <= MAX_MESSAGE && in.size() - body < *length) {
			return std::nullopt;
		}
		read = body;
		if (!length) {
			seeking = true;
		} else if (*length > MAX_MESSAGE) {
			skip = *length;
			r_skipped = *length;
		} else {
			read += *length;
			return in.substr(body, *length);
		}
	}
}

// A response is never answered, also one without a usable id (JSON-RPC 2.0,
// Response object).
void LspServer::handle(const std::string &p_body) {
	Ref<JSON> json;
	json.instantiate();
	const std::optional<String> text = json_text(p_body);
	if (!text || json->parse(*text) != OK) {
		reply_error(session->connection.out, Variant(), PARSE_ERROR, "The message is not JSON in UTF-8.");
		return;
	}
	const Variant data = json->get_data();
	const Dictionary message = dictionary(data);
	const Variant id = request_id(message.get("id", Variant()));
	const Variant method = message.get("method", Variant());
	const bool has_id = id.get_type() != Variant::NIL;
	if (!message.has("method") && (message.has("result") || message.has("error"))) {
		return;
	}
	if (data.get_type() != Variant::DICTIONARY || (message.has("id") && !has_id) || !message.has("method") || method.get_type() != Variant::STRING) {
		reply_error(session->connection.out, id, INVALID_REQUEST, "The message is no JSON-RPC request, notification or response.");
	} else if (has_id) {
		request(id, method, message.get("params", Variant()));
	} else {
		notification(method, dictionary(message.get("params", Variant())));
	}
}

// Before `initialize` and after `shutdown`, only `exit` is taken
// (LSP 3.17, Lifecycle Messages).
void LspServer::request(const Variant &p_id, const String &p_method, const Variant &p_params) {
	static const std::map<std::string, Variant (LspServer::*)(const Dictionary &)> QUERIES = {
		{ "textDocument/completion", &LspServer::on_completion },
		{ "textDocument/signatureHelp", &LspServer::on_signature_help },
		{ "textDocument/hover", &LspServer::on_hover },
		{ "textDocument/definition", &LspServer::on_definition },
		{ "textDocument/typeDefinition", &LspServer::on_type_definition },
		{ "textDocument/references", &LspServer::on_references },
		{ "textDocument/documentSymbol", &LspServer::on_document_symbol },
		{ "textDocument/documentHighlight", &LspServer::on_highlight },
		{ "textDocument/formatting", &LspServer::on_formatting },
		{ "textDocument/codeAction", &LspServer::on_code_action },
		{ "codeAction/resolve", &LspServer::on_resolve_action },
		{ "textDocument/prepareRename", &LspServer::on_prepare_rename },
		{ "textDocument/rename", &LspServer::on_rename },
	};
	std::string &out = session->connection.out;
	State &state = session->state;
	const Dictionary options = dictionary(dictionary(p_params).get("initializationOptions", Variant()));
	const Variant view_asked = options.get("view", Variant());
	if (p_method == "initialize" && state == State::Waiting && options.has("view") && view_asked != Variant("disk")) {
		reply_error(out, p_id, INVALID_PARAMS, vformat("The view %s is unknown. Ask for \"disk\", or for none to be on the human's view.", JSON::stringify(view_asked)));
	} else if (p_method == "initialize" && state == State::Waiting) {
		const Dictionary capabilities = dictionary(dictionary(p_params).get("capabilities", Variant()));
		const Dictionary document = dictionary(capabilities.get("textDocument", Variant()));
		const Dictionary files = dictionary(capabilities.get("workspace", Variant()));
		session->disk = options.has("view");
		// Of a rename's edits, which alone are annotated (LSP 3.17, RenameClientCapabilities).
		session->annotates = dictionary(files.get("workspaceEdit", Variant())).get("changeAnnotationSupport", Variant()).get_type() == Variant::DICTIONARY && dictionary(document.get("rename", Variant())).get("honorsChangeAnnotations", false) == Variant(true);
		session->snippets = dictionary(dictionary(document.get("completion", Variant())).get("completionItem", Variant())).get("snippetSupport", false) == Variant(true);
		const Variant resolved = dictionary(dictionary(document.get("codeAction", Variant())).get("resolveSupport", Variant())).get("properties", Variant());
		session->resolves_edits = resolved.get_type() == Variant::ARRAY && Array(resolved).has("edit");
		Dictionary sync;
		sync["openClose"] = true;
		sync["change"] = 2; // Incremental.
		sync["save"] = true;
		// As the script editor completes after them (editor/gui/code_editor.cpp:2077; ed1daf0).
		Dictionary completion;
		completion["triggerCharacters"] = Array::make(".", ",", "(", "=", "$", "@", "\"", "'");
		Dictionary signature_help;
		signature_help["triggerCharacters"] = Array::make("(", ",");
		Dictionary code_action;
		code_action["codeActionKinds"] = Array::make("quickfix");
		code_action["resolveProvider"] = true;
		Dictionary renaming;
		renaming["prepareProvider"] = true;
		Dictionary offered;
		offered["positionEncoding"] = "utf-16";
		offered["textDocumentSync"] = sync;
		offered["completionProvider"] = completion;
		offered["signatureHelpProvider"] = signature_help;
		offered["codeActionProvider"] = code_action;
		offered["renameProvider"] = renaming;
		for (const char *each : { "hoverProvider", "definitionProvider", "typeDefinitionProvider", "referencesProvider", "documentSymbolProvider", "documentHighlightProvider", "documentFormattingProvider" }) {
			offered[each] = true;
		}
		Dictionary info;
		info["name"] = "Petrichor";
		Dictionary result;
		result["capabilities"] = offered;
		result["serverInfo"] = info;
		reply(out, p_id, result);
		state = State::Running;
	} else if (state == State::Waiting) {
		reply_error(out, p_id, SERVER_NOT_INITIALIZED, "The server is not initialized.");
	} else if (p_method == "initialize" || state == State::ShutDown) {
		reply_error(out, p_id, INVALID_REQUEST, state == State::ShutDown ? "The server is shut down." : "The server is initialized already.");
	} else if (p_method == "shutdown") {
		state = State::ShutDown;
		reply(out, p_id, Variant());
	} else if (p_method == "petrichor/unsavedFiles") {
		// On either view (architecture §6.5).
		refresh();
		Array files;
		for (const std::string &file : held()) {
			Dictionary each;
			each["uri"] = uri_of(file);
			each["conflict"] = conflicts.count(file) > 0;
			files.push_back(each);
		}
		reply(out, p_id, files);
	} else if (p_method == "petrichor/run") {
		// A command of pet, on the files as they are now (architecture §6.5).
		const Dictionary params = dictionary(p_params);
		const Variant listed = params.get("args", Array());
		std::vector<std::string> args;
		for (int64_t i = 0; listed.get_type() == Variant::ARRAY && i < Array(listed).size(); i++) {
			args.push_back(to_utf8(Array(listed)[i].stringify()));
		}
		refresh();
		std::set<std::string> warned;
		for (const std::string &file : held()) {
			if (conflicts.count(file)) {
				warned.insert(file);
			}
		}
		const std::optional<std::pair<std::string, int>> ran = run(to_utf8(params.get("command", String()).stringify()), args, warned);
		if (ran) {
			Dictionary result;
			result["output"] = to_godot(ran->first);
			result["exitCode"] = ran->second;
			reply(out, p_id, result);
		} else {
			reply_error(out, p_id, INVALID_PARAMS, "The command is none of check, build and format.");
		}
	} else if (const auto query = QUERIES.find(to_utf8(p_method)); query != QUERIES.end()) {
		// On the files as they are now (architecture §6.5).
		if (session->disk) {
			refresh();
		}
		failure = String();
		Variant result;
		in_view([&] { result = (this->*query->second)(dictionary(p_params)); });
		if (failure.is_empty()) {
			reply(out, p_id, result);
		} else {
			reply_error(out, p_id, REQUEST_FAILED, failure);
		}
	} else {
		reply_error(out, p_id, METHOD_NOT_FOUND, "Unknown method: " + p_method);
	}
}

// One with a malformed field is ignored.
void LspServer::notification(const String &p_method, const Dictionary &p_params) {
	if (p_method == "exit") {
		session->connection.closing = true;
		return;
	}
	if (session->state != State::Running) {
		return;
	}
	if (p_method == "textDocument/didOpen") {
		opened(p_params);
	} else if (p_method == "textDocument/didChange") {
		changed(p_params);
	} else if (p_method == "textDocument/didClose") {
		closed(p_params);
	} else if (p_method == "textDocument/didSave" || p_method == "workspace/didChangeWatchedFiles") {
		refresh();
	}
}

// About once a second, the files compared with those last taken in, which
// refresh() then takes in on the main thread (architecture §6.5).
void LspServer::watch(const std::string &p_root) {
	std::unique_lock<std::mutex> lock(walking);
	while (!wake.wait_for(lock, std::chrono::seconds(1), [this] { return stopping; })) {
		lock.unlock();
		const std::map<std::string, FileStat> found = walk_project(p_root);
		lock.lock();
		if (found != walked) {
			stale = true;
		}
	}
}

// The files changed, added or removed since the last walk. A script, a scene,
// or a resource file in the text format, changed or added, is taken in as
// after a refactoring (PetEditorPlugin::reload_written()): the loaded scripts
// take the workspace's texts, the client's for those it has open, before what
// is loaded of the others is read again, so that whatever the order a rename's
// files are written in, nothing is read against a script's old members. Each
// script's bytecode is written (script_written()). Other files, and files
// removed, as the editor scans when it has focus again: a file loaded that
// changed is read again (editor/editor_node.cpp:1049-1058, 1291-1323,
// editor/file_system/editor_file_system.cpp:1597-1607, 1018-1025, 1076-1078;
// ed1daf0). Updating the file system otherwise would take the change as seen,
// and the scan would not read it again. A file as the editor last wrote or
// read it is no change: the walk finds the editor's own saves too, and reading
// a scene again clears its history.
//
// A file that Godot holds unsaved changes of is a conflict, told once to Godot
// and to every session (architecture §6.5). A tab or a scene keeps its
// changes, and its file time, so that Godot asks which to keep once it has
// focus (editor/script/script_editor_plugin.cpp:803-850, editor/editor_node.cpp:1550-1591;
// ed1daf0). Its file is not taken as seen, so that refactorings refuse to
// write over it. An edited resource is read back as Godot reads it once it has
// focus, which drops the edit. Godot does not ask about a scene whose file was
// removed, and saving writes it back (editor/editor_node.cpp:1565-1567).
void LspServer::refresh() {
	stale = false;
	std::map<std::string, FileStat> found = walk_project(root);
	std::set<std::string> changed;
	std::vector<std::string> removed;
	{
		std::lock_guard<std::mutex> lock(walking);
		if (found == walked) {
			return;
		}
		for (const auto &[path, stat] : found) {
			const auto was = walked.find(path);
			if (was == walked.end() || was->second != stat) {
				changed.insert(path);
			}
		}
		for (const auto &[path, stat] : walked) {
			if (!found.count(path)) {
				removed.push_back(path);
			}
		}
		walked = std::move(found);
	}
	bool scan = !removed.empty();
	std::set<std::string> outside;
	std::set<std::string> taken;
	const std::set<std::string> unsaved = godot_unsaved();
	PackedStringArray asked;
	PackedStringArray replaced;
	PackedStringArray deleted;
	for (const std::string &file : removed) {
		if ((unsaved.count(file) || edited_resource(file)) && conflicts.insert(file).second) {
			deleted.push_back(to_godot(file));
		}
	}
	for (const std::string &file : changed) {
		if (file_unchanged(file)) {
			conflicts.erase(file);
			continue;
		}
		outside.insert(file);
		const bool asks = unsaved.count(file) > 0;
		const bool edited = !asks && edited_resource(file);
		if (!asks && !edited) {
			conflicts.erase(file);
		} else if (conflicts.insert(file).second) {
			(asks ? asked : replaced).push_back(to_godot(file));
		}
		const std::string extension = lower_ascii(file.substr(file.find_last_of("./") + 1));
		if (extension == "pet" || extension == "tscn" || extension == "scn" || extension == "tres") {
			taken.insert(file);
			if (!asks) {
				file_seen(file);
			}
		} else {
			scan = true;
		}
	}
	PackedStringArray told_now;
	if (!asked.is_empty()) {
		told_now.push_back(vformat("Godot has unsaved changes to %s, which changed on disk. It asks which to keep once it has focus.", String(", ").join(asked)));
	}
	if (!replaced.is_empty()) {
		told_now.push_back(vformat("What changed on disk replaced Godot's unsaved changes to %s, as Godot itself does once it has focus.", String(", ").join(replaced)));
	}
	if (!deleted.is_empty()) {
		told_now.push_back(vformat("Godot has unsaved changes to %s, deleted on disk. Ctrl+S in Godot writes back what it has.", String(", ").join(deleted)));
	}
	if (!told_now.is_empty()) {
		const String message = String(" ").join(told_now);
		EditorInterface::get_singleton()->get_editor_toaster()->push_toast(message, EditorToaster::SEVERITY_WARNING);
		Session *asking = session;
		for (Session &each : sessions) {
			if (each.state == State::Running) {
				session = &each;
				show_message(message);
			}
		}
		session = asking;
	}
	if (outside.empty() && !scan) {
		return;
	}
	SceneIndex::get().files_changed(outside);
	editor_written(taken);
	for (const std::string &file : taken) {
		if (compiler::is_script_path(file)) {
			script_written(to_godot(file), debugger);
		}
	}
	if (scan) {
		disk_changed();
		EditorInterface::get_singleton()->get_resource_filesystem()->scan_sources();
	}
}

// Taken as the editor wrote them, so that they are not read back: that gives
// a resource it edited sub-resources other than those its undo history and
// inspector hold. A change made elsewhere before is left to refresh().
void LspServer::resources_saved() {
	if (!saving) {
		return;
	}
	const std::map<std::string, FileStat> after = walk_project(root);
	std::vector<std::string> written;
	{
		std::lock_guard<std::mutex> lock(walking);
		for (const auto &[path, stat] : after) {
			const auto was = saving->find(path);
			if (was == saving->end() || was->second != stat) {
				written.push_back(path);
				walked[path] = stat;
			}
		}
	}
	saving.reset();
	for (const std::string &path : written) {
		file_seen(path);
	}
}

std::set<std::string> LspServer::held() {
	std::set<std::string> files = godot_unsaved();
	for (auto each = conflicts.begin(); each != conflicts.end();) {
		if (files.count(*each) || edited_resource(*each)) {
			files.insert(*each);
			++each;
		} else {
			each = conflicts.erase(each);
		}
	}
	return files;
}

// A change that refresh() has not taken in has other stats than the walk.
bool LspServer::conflicts_with_disk(const std::string &p_path) {
	if (conflicts.count(p_path)) {
		return true;
	}
	const std::optional<FileStat> now = file_stat(p_path);
	std::optional<FileStat> was;
	{
		std::lock_guard<std::mutex> lock(walking);
		const auto found = walked.find(p_path);
		if (found != walked.end()) {
			was = found->second;
		}
	}
	return now != was && !file_unchanged(p_path);
}

void LspServer::opened(const Dictionary &p_params) {
	const Dictionary item = dictionary(p_params.get("textDocument", Variant()));
	const Variant uri = item.get("uri", Variant());
	const Variant text = item.get("text", Variant());
	const std::optional<int64_t> version = integer(item.get("version", Variant()), INT32_MIN, INT32_MAX);
	const std::string path = script_path(uri);
	if (path.empty() || text.get_type() != Variant::STRING || !version) {
		return;
	}
	Document &document = session->documents[path];
	document.versions[uri] = *version;
	document.published = 0;
	document.text = to_utf8(text);
	edited++;
	if (!session->disk) {
		set_client_script(path, session, document.text);
	}
}

// Each change applies to the text that the one before it left (LSP 3.17,
// DidChangeTextDocumentParams). One file open under two spellings of its URI
// has one text, which the changes under either change.
void LspServer::changed(const Dictionary &p_params) {
	const Dictionary item = dictionary(p_params.get("textDocument", Variant()));
	const Variant uri = item.get("uri", Variant());
	const auto found = session->documents.find(script_path(uri));
	const std::optional<int64_t> version = integer(item.get("version", Variant()), INT32_MIN, INT32_MAX);
	const Variant changes = p_params.get("contentChanges", Variant());
	if (found == session->documents.end() || !found->second.versions.count(uri) || !version || changes.get_type() != Variant::ARRAY) {
		return;
	}
	std::string text = found->second.text;
	const Array list = changes;
	for (int64_t i = 0; i < list.size(); i++) {
		const Dictionary change = dictionary(list[i]);
		const Variant replacement = change.get("text", Variant());
		if (replacement.get_type() != Variant::STRING) {
			return;
		}
		if (!change.has("range")) {
			text = to_utf8(replacement);
			continue;
		}
		const Dictionary span = dictionary(change["range"]);
		const std::optional<compiler::Utf16Position> start = position(span.get("start", Variant()));
		const std::optional<compiler::Utf16Position> end = position(span.get("end", Variant()));
		if (!start || !end) {
			return;
		}
		const uint32_t from = compiler::utf16_offset(text, *start);
		const uint32_t to = compiler::utf16_offset(text, *end);
		text.replace(std::min(from, to), std::max(from, to) - std::min(from, to), to_utf8(replacement));
	}
	found->second.versions[uri] = *version;
	found->second.text = std::move(text);
	edited++;
	if (!session->disk) {
		set_client_script(found->first, session, found->second.text);
	}
}

// Its diagnostics cleared; closed under its last spelling, given back.
void LspServer::closed(const Dictionary &p_params) {
	const Variant uri = dictionary(p_params.get("textDocument", Variant())).get("uri", Variant());
	const auto found = session->documents.find(script_path(uri));
	if (found == session->documents.end() || !found->second.versions.erase(uri)) {
		return;
	}
	Dictionary params;
	params["uri"] = uri;
	params["diagnostics"] = Array();
	notify("textDocument/publishDiagnostics", params);
	if (found->second.versions.empty()) {
		const std::string path = found->first;
		session->documents.erase(found);
		edited++;
		release(path);
	}
}

// A script that the session closed, on the human's view, goes to the text of
// another session that has it open, else to its tab or its file (architecture
// §2.4). What the analysis kept for a script with no file goes with its text. A
// loaded script that took the session's unsaved text, as reading a scene or a
// resource again gives it, takes the text the workspace has now, as the
// script editor reloads a script whose tab's changes are discarded
// (editor/script/script_editor_plugin.cpp:607-611; ed1daf0).
void LspServer::release(const std::string &p_path) {
	if (!session->disk) {
		drop_client_script(p_path, session);
	}
	if (!FileAccess::file_exists(to_godot(p_path))) {
		analysis().forget(p_path);
	}
	const Ref<Script> loaded = session->disk ? Ref<Script>() : Ref<Script>(ResourceLoader::get_singleton()->get_cached_ref(to_godot(p_path)));
	if (loaded.is_valid() && loaded->get_source_code() != to_godot(workspace().script_source(p_path).value_or(std::string()))) {
		editor_written({ p_path });
	}
}

// What the session sees: its view, the human's or the disk's, with the texts
// of its documents where they differ (architecture §2.4). The analysis reads
// it while `p_do` runs. A session on the human's view that changed its
// documents last sees the workspace.
void LspServer::in_view(const std::function<void()> &p_do) {
	const compiler::ProjectApi &under = session->disk ? static_cast<const compiler::ProjectApi &>(SceneIndex::get()) : workspace();
	compiler::Overlay own(under);
	for (const auto &[path, document] : session->documents) {
		if (under.script_source(path) != document.text) {
			own.scripts[path] = document.text;
		}
	}
	view = own.scripts.empty() ? &under : &own;
	{
		const compiler::Analysis::View viewing(analysis(), *view, session->disk);
		p_do();
	}
	view = nullptr;
}

// The diagnostics that the script editor shows for the session's text: the
// script's own, with those of the scenes that place it (architecture §2.4).
void LspServer::publish(const std::string &p_path, Document &r_document) {
	const uint64_t now = revision();
	const std::string &text = r_document.text;
	std::vector<compiler::Diagnostic> found;
	in_view([&] {
		const compiler::Analysis::Pass pass(analysis());
		const std::shared_ptr<const compiler::CompileResult> result = analysis().compile(p_path, text);
		const std::vector<compiler::Diagnostic> placed = analysis().placed_errors(p_path);
		found = result->errors;
		found.insert(found.end(), placed.begin(), placed.end());
		found.insert(found.end(), result->warnings.begin(), result->warnings.end());
	});
	std::vector<uint32_t> offsets;
	for (const compiler::Diagnostic &each : found) {
		offsets.push_back(each.begin);
		offsets.push_back(std::max(each.begin, each.end));
	}
	const std::vector<compiler::Utf16Position> at = compiler::utf16_positions(text, offsets);
	Array diagnostics;
	for (size_t i = 0; i < found.size(); i++) {
		diagnostics.push_back(diagnostic(found[i], range(at[2 * i], at[2 * i + 1])));
	}
	for (const auto &[uri, version] : r_document.versions) {
		Dictionary params;
		params["uri"] = uri;
		params["version"] = version;
		params["diagnostics"] = diagnostics;
		notify("textDocument/publishDiagnostics", params);
	}
	r_document.published = now;
}

void LspServer::notify(const char *p_method, const Dictionary &p_params) {
	Dictionary message;
	message["method"] = p_method;
	message["params"] = p_params;
	send(session->connection.out, message);
}

void LspServer::show_message(const String &p_message) {
	Dictionary params;
	params["type"] = 2; // Warning.
	params["message"] = p_message;
	notify("window/showMessage", params);
}

// The scripts the session had open are given back as it closes them. On the
// human's view, the editor checks again at once, as after a change on disk.
std::list<LspServer::Session>::iterator LspServer::end(std::list<Session>::iterator p_session) {
	session = &*p_session;
	session->connection.peer->disconnect_from_host();
	const std::map<std::string, Document> documents = std::move(session->documents);
	session->documents.clear();
	for (const auto &[path, document] : documents) {
		release(path);
	}
	const bool given = !documents.empty() && !session->disk;
	session = nullptr;
	if (given) {
		editor_changed();
	}
	return sessions.erase(p_session);
}

// The session's own for a script it has open; otherwise with long names.
String LspServer::uri_of(const std::string &p_path) const {
	const auto open = session->documents.find(p_path);
	if (open != session->documents.end()) {
		return open->second.versions.begin()->first;
	}
	const std::string path = project_root() + p_path.substr(6);
	std::string uri = path.compare(0, 1, "/") == 0 ? "file://" : "file:///";
	for (const unsigned char c : path) {
		if (std::isalnum(c) || std::strchr("-._~/", c)) {
			uri += char(c);
		} else {
			uri += '%';
			uri += "0123456789ABCDEF"[c >> 4];
			uri += "0123456789ABCDEF"[c & 15];
		}
	}
	return to_godot(uri);
}

Dictionary LspServer::location(const std::string &p_path, const std::string &p_text, uint32_t p_begin, uint32_t p_end) const {
	Dictionary result;
	result["uri"] = uri_of(p_path);
	result["range"] = range_of(p_text, p_begin, p_end);
	return result;
}

// Against the texts the session sees, the edits in `p_confirm`, by file and
// start, annotated to need confirmation. None where a file that the session
// does not have open is held, which `r_held` lists: the client edits the file
// (architecture §2.4). That is anything Godot holds unsaved or has not seen of
// it, and on the human's view also another session's text.
Dictionary LspServer::edit_of(const std::map<std::string, std::vector<compiler::CheckedEdit>> &p_edits, Held &r_held, const std::set<std::pair<std::string, uint32_t>> &p_confirm) const {
	std::set<std::string> files;
	std::set<std::string> own;
	for (const auto &[file, in_file] : p_edits) {
		files.insert(file);
	}
	for (const auto &[file, document] : session->documents) {
		own.insert(file);
	}
	r_held = Held::of(files, own, !session->disk);
	Array changes;
	for (const auto &[file, in_file] : p_edits) {
		const std::string text = view->script_source(file).value_or(std::string());
		const auto open = session->documents.find(file);
		std::vector<compiler::CheckedEdit> ordered = in_file;
		std::stable_sort(ordered.begin(), ordered.end(), [](const compiler::CheckedEdit &p_a, const compiler::CheckedEdit &p_b) { return p_a.begin < p_b.begin; });
		Array text_edits;
		for (const compiler::CheckedEdit &edit : ordered) {
			Dictionary each = text_edit(text, edit.begin, edit.end, edit.new_text);
			if (p_confirm.count({ file, edit.begin })) {
				each["annotationId"] = "candidate";
			}
			text_edits.push_back(each);
		}
		Dictionary document;
		document["uri"] = uri_of(file);
		document["version"] = open == session->documents.end() ? Variant() : Variant(open->second.versions.begin()->second);
		Dictionary change;
		change["textDocument"] = document;
		change["edits"] = text_edits;
		changes.push_back(change);
	}
	if (!r_held.empty()) {
		return Dictionary();
	}
	Dictionary result;
	result["documentChanges"] = changes;
	if (!p_confirm.empty()) {
		Dictionary candidate;
		candidate["label"] = "Candidates";
		candidate["description"] = "Names that may refer to it, but do not resolve to it.";
		candidate["needsConfirmation"] = true;
		Dictionary annotations;
		annotations["candidate"] = candidate;
		result["changeAnnotations"] = annotations;
	}
	return result;
}

// The editor's list (spec §13.2). An insertion ending in "(" is a snippet
// with the cursor inside, as GDScript's server makes it, or without the "("
// for a client without snippets
// (modules/gdscript/language_server/gdscript_language_protocol.cpp:559-569;
// ed1daf0). Each replaces what the editor replaces; in a string, it is
// quoted as the string is, and replaces the closing quote after the cursor
// (scene/gui/code_edit.cpp:2614-2618, 3916-3927). Typed right after "(", the
// list is empty unless it is forced, as in the editor (:3832-3835).
Variant LspServer::on_completion(const Dictionary &p_params) {
	const std::optional<Place> at = place_of(*view, p_params, true);
	if (!at) {
		return Variant();
	}
	std::string marked = at->text;
	marked.insert(at->offset, CURSOR);
	const compiler::Completion found = completion(to_godot(at->path), marked);
	const bool invoked = integer(dictionary(p_params.get("context", Variant())).get("triggerKind", Variant()), 0, 3) == std::optional<int64_t>(1);
	Array items;
	if (found.force || invoked || at->offset == 0 || at->text[at->offset - 1] != '(') {
		std::string quote;
		const uint32_t start = completion_start(at->text, at->offset, quote);
		quote = quote.substr(0, 1);
		const bool closes = !quote.empty() && at->text.compare(at->offset, 1, quote) == 0;
		for (const compiler::CompletionItem &item : found.items) {
			Dictionary each;
			std::string insert = item.insert;
			if (!quote.empty()) {
				const bool quoted = insert.size() > 1 && (insert[0] == '"' || insert[0] == '\'') && insert.back() == insert[0];
				insert = quote + (quoted ? insert.substr(1, insert.size() - 2) : insert) + quote;
			}
			const std::string filter = insert;
			if (!insert.empty() && insert.back() == '(') {
				insert.pop_back();
				if (session->snippets) {
					std::string escaped;
					for (const char c : insert) {
						escaped += c == '$' || c == '}' || c == '\\' ? std::string("\\") + c : std::string(1, c);
					}
					insert = escaped + "($1)";
					each["insertTextFormat"] = 2; // Snippet.
				}
			}
			// The editor's order: by where the name is declared, then by name.
			int rank = 1 << 10;
			if (item.place == compiler::CompletionItem::Place::Local) {
				rank = 0;
			} else if (item.place == compiler::CompletionItem::Place::Member) {
				rank = (1 << 8) | item.depth;
			} else if (item.place == compiler::CompletionItem::Place::Project) {
				rank = 1 << 9;
			}
			each["label"] = to_godot(item.name);
			each["kind"] = COMPLETION_KINDS[int(item.kind)];
			each["sortText"] = to_godot(std::to_string(10000 + rank) + item.name);
			each["filterText"] = to_godot(filter);
			each["textEdit"] = text_edit(at->text, start, at->offset + (closes ? 1 : 0), insert);
			items.push_back(each);
		}
	}
	Dictionary result;
	result["isIncomplete"] = false;
	result["items"] = items;
	return result;
}

// The editor's call hint, one signature for each of its lines, the
// parameter it marks as the active one.
Variant LspServer::on_signature_help(const Dictionary &p_params) {
	const std::optional<Place> at = place_of(*view, p_params, true);
	if (!at) {
		return Variant();
	}
	std::string marked = at->text;
	marked.insert(at->offset, CURSOR);
	const std::string hint = completion(to_godot(at->path), marked).call_hint;
	if (hint.empty()) {
		return Variant();
	}
	Array signatures;
	for (size_t begin = 0; begin <= hint.size();) {
		const size_t end = std::min(hint.find('\n', begin), hint.size());
		std::string label = hint.substr(begin, end - begin);
		begin = end + 1;
		Dictionary signature;
		Array parameters;
		const size_t open = label.find(CURSOR);
		const size_t close = open == std::string::npos ? std::string::npos : label.find(CURSOR, open + 3);
		if (close != std::string::npos) {
			const uint32_t from = utf16_length(label.substr(0, open));
			Dictionary parameter;
			parameter["label"] = Array::make(from, from + utf16_length(label.substr(open + 3, close - open - 3)));
			parameters.push_back(parameter);
			signature["activeParameter"] = 0;
			label.erase(close, 3);
			label.erase(open, 3);
		}
		signature["label"] = to_godot(label);
		signature["parameters"] = parameters;
		signatures.push_back(signature);
	}
	Dictionary result;
	result["signatures"] = signatures;
	result["activeSignature"] = 0;
	return result;
}

// What the editor's lookup gives, as Markdown: the declaration, as far as it
// is known, then its documentation.
Variant LspServer::on_hover(const Dictionary &p_params) {
	const std::optional<Place> at = place_of(*view, p_params, true);
	const std::optional<compiler::Lookup> found = at ? lookup_name(to_godot(at->path), at->text, at->offset) : std::nullopt;
	if (!found) {
		return Variant();
	}
	std::string text = "```petrichor\n" + found->declaration + "\n```";
	const compiler::DocComment &comment = found->comment;
	if (comment.deprecated) {
		text += "\n\n**Deprecated**" + (comment.deprecated->empty() ? "" : ": " + *comment.deprecated);
	}
	if (comment.experimental) {
		text += "\n\n**Experimental**" + (comment.experimental->empty() ? "" : ": " + *comment.experimental);
	}
	for (const std::string *paragraph : { &comment.brief, &comment.description }) {
		if (!paragraph->empty()) {
			text += "\n\n" + *paragraph;
		}
	}
	Dictionary contents;
	contents["kind"] = "markdown";
	contents["value"] = to_godot(text);
	Dictionary result;
	result["contents"] = contents;
	return result;
}

// A declaration of this language or another at its name; none of the
// engine's. A node path leads to the node's header in each text scene that
// places the script.
Variant LspServer::on_definition(const Dictionary &p_params) {
	const std::optional<Place> at = place_of(*view, p_params, true);
	if (!at) {
		return Variant();
	}
	const std::optional<compiler::Lookup> found = lookup_name(to_godot(at->path), at->text, at->offset);
	if (found && !found->path.empty()) {
		return location(found->path, view->script_source(found->path).value_or(std::string()), found->begin, found->end);
	}
	std::vector<compiler::DeclId> nodes;
	{
		const compiler::Analysis::Pass pass(analysis());
		for (const compiler::Reference &reference : analysis().compile(at->path, at->text)->references) {
			if (reference.target.kind == compiler::DeclId::Kind::Node && reference.begin <= at->offset && at->offset <= reference.end) {
				nodes.push_back(reference.target);
			}
		}
	}
	Array headers;
	for (const compiler::DeclId &node : nodes) {
		const std::optional<std::string> text = is_text(node.scope) ? view->script_source(node.scope) : std::nullopt;
		const std::optional<uint32_t> header = text ? compiler::node_header(*text, node.name) : std::nullopt;
		if (header) {
			headers.push_back(location(node.scope, *text, *header, *header));
		}
	}
	return headers.is_empty() ? Variant() : Variant(headers);
}

Variant LspServer::on_type_definition(const Dictionary &p_params) {
	const std::optional<Place> at = place_of(*view, p_params, true);
	Array found;
	for (const compiler::Lookup &each : at ? type_definition(at->path, at->text, at->offset) : std::vector<compiler::Lookup>()) {
		found.push_back(location(each.path, view->script_source(each.path).value_or(std::string()), each.begin, each.end));
	}
	return found;
}

// Without candidates, which a Location cannot mark (architecture §2.4). A
// place in a scene or a resource file is where renaming rewrites the name
// in its text, as compiler::rename() finds it; the line it would refuse at;
// or the file's start, of a binary one.
Variant LspServer::on_references(const Dictionary &p_params) {
	const std::optional<Place> at = place_of(*view, p_params, true);
	const std::optional<std::vector<compiler::Occurrence>> found = at ? find_references(at->path, at->offset) : std::nullopt;
	if (!found) {
		return Variant();
	}
	const bool declarations = dictionary(p_params.get("context", Variant())).get("includeDeclaration", false) == Variant(true);
	bool signal = false;
	std::string name;
	for (const compiler::Occurrence &each : *found) {
		signal = signal || each.reference.declares == compiler::SymbolKind::Signal;
		if (each.file == at->path && each.line > 0 && each.reference.begin <= at->offset && at->offset <= each.reference.end) {
			name = at->text.substr(each.reference.begin, each.reference.end - each.reference.begin);
		}
	}
	std::map<std::string, std::string> texts;
	const auto text_of = [&](const std::string &p_file) -> const std::string & {
		auto known = texts.find(p_file);
		if (known == texts.end()) {
			known = texts.emplace(p_file, is_text(p_file) ? view->script_source(p_file).value_or(std::string()) : std::string()).first;
		}
		return known->second;
	};
	std::set<std::tuple<std::string, uint32_t, uint32_t>> places;
	for (const compiler::Occurrence &each : *found) {
		const compiler::Reference &reference = each.reference;
		if (each.candidate || (each.declaration && !declarations)) {
			continue;
		}
		if (each.line > 0) {
			places.emplace(each.file, reference.begin, reference.end);
			continue;
		}
		if (reference.target.kind == compiler::DeclId::Kind::Node) {
			const uint32_t header = compiler::node_header(text_of(each.file), reference.target.name).value_or(0);
			places.emplace(each.file.substr(0, each.file.find("::")), header, header);
			continue;
		}
		for (const auto &[file, stored] : compiler::stored_names(reference)) {
			const std::string own = file.substr(0, file.find("::"));
			const compiler::LocatedNames located = is_text(file) ? compiler::locate_names(text_of(own), { stored }, signal, name, std::string()) : compiler::LocatedNames();
			for (const compiler::CheckedEdit &edit : located.edits) {
				places.emplace(own, edit.begin, edit.end);
			}
			if (located.edits.empty()) {
				const uint32_t line = located.line > 0 ? compiler::line_starts_of(text_of(own))[located.line - 1] : 0;
				places.emplace(own, line, line);
			}
		}
	}
	Array result;
	for (const auto &[file, begin, end] : places) {
		result.push_back(location(file, text_of(file), begin, end));
	}
	return result;
}

Variant LspServer::on_document_symbol(const Dictionary &p_params) {
	const std::optional<Place> at = place_of(*view, p_params, false);
	if (!at) {
		return Variant();
	}
	const compiler::Symbol file = compiler::outline(at->text, at->path.substr(at->path.rfind('/') + 1));
	std::vector<uint32_t> offsets;
	const std::function<void(const compiler::Symbol &)> gather = [&](const compiler::Symbol &p_symbol) {
		offsets.insert(offsets.end(), { p_symbol.begin, p_symbol.end, p_symbol.name_begin, p_symbol.name_end });
		for (const compiler::Symbol &child : p_symbol.children) {
			gather(child);
		}
	};
	gather(file);
	const std::vector<compiler::Utf16Position> places = compiler::utf16_positions(at->text, offsets);
	size_t next = 0;
	// One without a name is what a syntax error left, which LSP does not take.
	const std::function<Variant(const compiler::Symbol &)> made = [&](const compiler::Symbol &p_symbol) -> Variant {
		Dictionary symbol;
		symbol["name"] = to_godot(p_symbol.name);
		symbol["kind"] = SYMBOL_KINDS[int(p_symbol.kind)];
		symbol["range"] = range(places[next], places[next + 1]);
		symbol["selectionRange"] = range(places[next + 2], places[next + 3]);
		next += 4;
		Array children;
		for (const compiler::Symbol &child : p_symbol.children) {
			const Variant each = made(child);
			if (each.get_type() != Variant::NIL) {
				children.push_back(each);
			}
		}
		symbol["children"] = children;
		return p_symbol.name.empty() ? Variant() : Variant(symbol);
	};
	return Array::make(made(file));
}

// The names in the script that refer to what the name at the position does,
// from its own references; each one of a node path, which refers to the node
// in each scene, once.
Variant LspServer::on_highlight(const Dictionary &p_params) {
	const std::optional<Place> at = place_of(*view, p_params, true);
	if (!at) {
		return Variant();
	}
	std::set<std::pair<uint32_t, uint32_t>> names;
	{
		const compiler::Analysis::Pass pass(analysis());
		const std::shared_ptr<const compiler::CompileResult> result = analysis().compile(at->path, at->text);
		const compiler::Reference *name = compiler::reference_at(*result, at->offset);
		if (!name || name->target.kind == compiler::DeclId::Kind::Unresolved) {
			return Variant();
		}
		for (const compiler::Reference &reference : result->references) {
			if (reference.target == name->target && reference.end > reference.begin) {
				names.emplace(reference.begin, reference.end);
			}
		}
	}
	Array found;
	for (const auto &[begin, end] : names) {
		Dictionary each;
		each["range"] = range_of(at->text, begin, end);
		found.push_back(each);
	}
	return found;
}

// As the editor's Format, which fails with the reason on a syntax error.
Variant LspServer::on_formatting(const Dictionary &p_params) {
	const std::optional<Place> at = place_of(*view, p_params, false);
	if (!at) {
		return Variant();
	}
	std::string error;
	const std::optional<std::string> formatted = compiler::format(at->text, &error);
	if (!formatted) {
		failure = to_godot(error);
		return Variant();
	}
	const std::vector<compiler::Edit> edits = compiler::format_edits(at->text, *formatted);
	std::vector<uint32_t> offsets;
	for (const compiler::Edit &edit : edits) {
		offsets.push_back(edit.begin);
		offsets.push_back(edit.end);
	}
	const std::vector<compiler::Utf16Position> places = compiler::utf16_positions(at->text, offsets);
	Array result;
	for (size_t i = 0; i < edits.size(); i++) {
		Dictionary each;
		each["range"] = range(places[2 * i], places[2 * i + 1]);
		each["newText"] = to_godot(edits[i].text);
		result.push_back(each);
	}
	return result;
}

// The quick fixes on the lines of the range, as the script editor lists them
// (PetEditorPlugin::list_fixes_at()): first those of the errors the range
// meets, then of the others by position, each once; after them, fixing all
// of each automatic kind among them where the editor offers it.
Variant LspServer::on_code_action(const Dictionary &p_params) {
	const std::optional<Place> at = place_of(*view, p_params, false);
	const Dictionary span = dictionary(p_params.get("range", Variant()));
	const std::optional<compiler::Utf16Position> start = position(span.get("start", Variant()));
	const std::optional<compiler::Utf16Position> end = position(span.get("end", Variant()));
	const Variant only = dictionary(p_params.get("context", Variant())).get("only", Variant());
	if (!at || !start || !end) {
		return Variant();
	}
	Array actions;
	if (only.get_type() == Variant::ARRAY && !Array(only).has("quickfix")) {
		return actions;
	}
	const uint32_t from = compiler::utf16_offset(at->text, *start);
	const uint32_t to = compiler::utf16_offset(at->text, *end);
	const uint32_t first = compiler::utf16_offset(at->text, { start->line, 0 });
	const uint32_t last = compiler::utf16_offset(at->text, { end->line, UINT32_MAX });
	std::shared_ptr<const compiler::CompileResult> result;
	std::vector<compiler::Diagnostic> errors;
	uint64_t revision = 0;
	{
		const compiler::Analysis::Pass pass(analysis());
		result = analysis().compile(at->path, at->text);
		errors = fixable(result->errors, at->path);
		revision = analysis().computed(at->path);
	}
	std::vector<const compiler::Diagnostic *> on_lines;
	for (const compiler::Diagnostic &error : errors) {
		if (first <= error.begin && error.begin <= last) {
			on_lines.push_back(&error);
		}
	}
	std::stable_partition(on_lines.begin(), on_lines.end(), [&](const compiler::Diagnostic *p_error) { return p_error->begin <= to && from <= p_error->end; });
	// `p_data` names the fix, which the stamp of the check and the files it
	// edits complete for codeAction/resolve. One refused for unsaved files
	// says so in its title and has no edit: VS Code does not show a disabled
	// action among the quick fixes, nor its reason.
	const auto add = [&](const String &p_title, const FileEdits &p_edits, const compiler::Diagnostic *p_error, bool p_preferred, Dictionary p_data) {
		Held held;
		const Dictionary edit = edit_of(p_edits, held);
		Dictionary action;
		action["title"] = held.empty() ? p_title : vformat("%s (%s)", p_title, held_note(held));
		action["kind"] = "quickfix";
		if (p_error) {
			action["diagnostics"] = Array::make(diagnostic(*p_error, range_of(at->text, p_error->begin, p_error->end)));
		}
		if (p_preferred) {
			action["isPreferred"] = true;
		}
		Array files = Array::make(to_godot(at->path));
		for (const auto &[file, in_file] : p_edits) {
			files.push_back(to_godot(file));
		}
		p_data["uri"] = uri_of(at->path);
		p_data["revision"] = int64_t(revision);
		p_data["files"] = files;
		action["data"] = p_data;
		if (held.empty() && !session->resolves_edits) {
			action["edit"] = edit;
		}
		actions.push_back(action);
	};
	std::vector<const compiler::Fix *> listed;
	std::vector<compiler::FixKind> kinds;
	for (const compiler::Diagnostic *error : on_lines) {
		for (const compiler::Fix &fix : error->fixes) {
			if (std::any_of(listed.begin(), listed.end(), [&](const compiler::Fix *p_listed) { return same_fix(*p_listed, fix); })) {
				continue;
			}
			listed.push_back(&fix);
			FileEdits edits;
			for (const compiler::Fix::Edit &edit : fix.edits) {
				edits[edit.file].push_back(edit.change);
			}
			Dictionary data;
			data["title"] = to_godot(fix.title);
			data["begin"] = int64_t(error->begin);
			data["end"] = int64_t(error->end);
			data["message"] = to_godot(error->message);
			data["file"] = to_godot(error->file);
			add(to_godot(fix.title), edits, error, fix.automatic(), data);
			if (fix.automatic() && std::find(kinds.begin(), kinds.end(), fix.kind) == kinds.end()) {
				kinds.push_back(fix.kind);
			}
		}
	}
	for (const compiler::FixKind kind : kinds) {
		if (fixes_all(errors, at->path, kind)) {
			Dictionary data;
			data["kind"] = int64_t(kind);
			add(vformat("Fix all '%s' in file", compiler::FIX_KINDS[size_t(kind)].name), { { at->path, compiler::fix_all(*result, kind) } }, nullptr, false, data);
		}
	}
	return actions;
}

// The edit of a code action chosen, found again from its data as the editor
// finds a fix it listed (fix_again()), against the texts as they are now: the
// client applies it to them without checking their versions. Where the fix
// is not found or is refused, the action has no edit, and the client is told.
Variant LspServer::on_resolve_action(const Dictionary &p_params) {
	Dictionary action = p_params.duplicate();
	action.erase("edit");
	const Dictionary data = dictionary(p_params.get("data", Variant()));
	const std::string path = script_path(data.get("uri", Variant()));
	const std::optional<int64_t> revision = integer(data.get("revision", Variant()), 0, int64_t(1) << 53);
	const Variant files = data.get("files", Variant());
	const Variant title = data.get("title", Variant());
	const std::optional<int64_t> kind = integer(data.get("kind", Variant()), 0, int64_t(std::size(compiler::FIX_KINDS)) - 1);
	if (path.empty() || !revision || files.get_type() != Variant::ARRAY || (title.get_type() != Variant::STRING && !kind)) {
		return action;
	}
	compiler::Stamp stamp{ uint64_t(*revision), {} };
	const Array listed = files;
	for (int64_t i = 0; i < listed.size(); i++) {
		stamp.files.insert(to_utf8(String(listed[i])));
	}
	compiler::Diagnostic error;
	error.begin = uint32_t(integer(data.get("begin", Variant()), 0, UINT32_MAX).value_or(0));
	error.end = uint32_t(integer(data.get("end", Variant()), 0, UINT32_MAX).value_or(0));
	error.message = to_utf8(String(data.get("message", String())));
	error.file = to_utf8(String(data.get("file", String())));
	const bool one = title.get_type() == Variant::STRING;
	const FileEdits changes = fix_again(path, stamp, one ? &error : nullptr, one ? to_utf8(title) : std::string(), compiler::FixKind(kind.value_or(0)));
	if (changes.empty()) {
		show_message("The project changed. Ask for the quick fixes again.");
		return action;
	}
	Held held;
	const Dictionary edit = edit_of(changes, held);
	if (!held.empty()) {
		show_message(held.reason());
		return action;
	}
	action["edit"] = edit;
	return action;
}

// The name, where the editor asks for a new one (PetEditorPlugin::ask_name());
// otherwise why it renames nothing there.
Variant LspServer::on_prepare_rename(const Dictionary &p_params) {
	const std::optional<Place> at = place_of(*view, p_params, true);
	if (!at) {
		return Variant();
	}
	const compiler::RenamedName name = renamed_name(at->path, at->offset);
	if (name.refusal) {
		failure = refused_rename({ *name.refusal }, at->path);
		return Variant();
	}
	Dictionary result;
	result["range"] = range_of(at->text, name.begin, name.end);
	result["placeholder"] = to_godot(at->text.substr(name.begin, name.end - name.begin));
	return result;
}

// What the editor's rename changes (PetEditorPlugin::rename_to()), as one edit
// that the client previews, applies and undoes (architecture §2.4). The
// candidates need confirmation, so they start unchecked. A client that cannot
// mark them does not get them.
Variant LspServer::on_rename(const Dictionary &p_params) {
	const std::optional<Place> at = place_of(*view, p_params, true);
	const Variant name = p_params.get("newName", Variant());
	if (!at || name.get_type() != Variant::STRING) {
		return Variant();
	}
	const compiler::Rename found = rename(at->path, at->offset, to_utf8(name));
	if (!found.refusals.empty()) {
		failure = refused_rename(found.refusals, at->path);
		return Variant();
	}
	FileEdits changes;
	std::set<std::pair<std::string, uint32_t>> candidates;
	for (const compiler::RenameEdit &each : found.edits) {
		if (each.occurrence.candidate && !session->annotates) {
			continue;
		}
		if (each.occurrence.candidate) {
			candidates.emplace(each.occurrence.file, each.edit.begin);
		}
		changes[each.occurrence.file].push_back(each.edit);
	}
	Held held;
	const Dictionary edit = edit_of(changes, held, candidates);
	failure = held.reason();
	return edit;
}

} // namespace petrichor
