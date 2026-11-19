#include "adapter/editor/lsp_server.h"

#include "adapter/editor/scene_index.h"
#include "adapter/editor/script_compiler.h"
#include "adapter/pet_compiled.h"
#include "compiler/analysis.h"
#include "compiler/lexer.h"
#include "runtime/text.h"

#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/editor_debugger_session.hpp>
#include <godot_cpp/classes/editor_file_system.hpp>
#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/editor_settings.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/json.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstring>
#include <optional>

using namespace godot;

namespace petrichor {

namespace {

constexpr size_t MAX_MESSAGE = size_t(16) << 20;
constexpr size_t MAX_HEADER = size_t(64) << 10;
constexpr size_t MAX_UNSENT = size_t(128) << 20; // For a client that reads nothing.
constexpr size_t MAX_REFUSED = 4;
constexpr int32_t CHUNK = 4 << 20; // Read or sent in a frame.
constexpr uint64_t FRAME_BUDGET = 50; // In ms, for the messages of a frame.

// JSON-RPC 2.0 and LSP 3.17.
constexpr int PARSE_ERROR = -32700;
constexpr int INVALID_REQUEST = -32600;
constexpr int METHOD_NOT_FOUND = -32601;
constexpr int SERVER_NOT_INITIALIZED = -32002;
constexpr int REQUEST_FAILED = -32803;

String info_path() {
	return ProjectSettings::get_singleton()->globalize_path("res://.godot/petrichor/lsp.json");
}

std::string lower_ascii(std::string p_text) {
	for (char &c : p_text) {
		if (c >= 'A' && c <= 'Z') {
			c = char(c - 'A' + 'a');
		}
	}
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

// The last Content-Length in a header, so that the header after a broken
// message's body is still found; nullopt without one of up to MAX_MESSAGE.
std::optional<size_t> content_length(const std::string &p_header) {
	const std::string header = lower_ascii(p_header);
	const size_t name = header.rfind("content-length:");
	if (name == std::string::npos) {
		return std::nullopt;
	}
	size_t at = header.find_first_not_of(" \t", name + 15);
	const size_t digits = at;
	size_t length = 0;
	for (; at < header.size() && header[at] >= '0' && header[at] <= '9' && at - digits < 12; at++) {
		length = length * 10 + size_t(header[at] - '0');
	}
	const size_t after = header.find_first_not_of(" \t", at);
	if (at == digits || (after != std::string::npos && header[after] != '\r') || length > MAX_MESSAGE) {
		return std::nullopt;
	}
	return length;
}

// The body of the next message that has all arrived after `r_read`. A header
// without a usable Content-Length is passed over, and what follows is read
// as the next header.
std::optional<std::string> next_message(const std::string &p_in, size_t &r_read) {
	while (true) {
		const size_t end = p_in.find("\r\n\r\n", r_read);
		if (end == std::string::npos) {
			if (p_in.size() - r_read > MAX_HEADER) {
				r_read = p_in.size() - 3;
			}
			return std::nullopt;
		}
		const std::optional<size_t> length = content_length(p_in.substr(r_read, end - r_read));
		const size_t body = end + 4;
		if (!length) {
			r_read = body;
			continue;
		}
		if (p_in.size() - body < *length) {
			return std::nullopt;
		}
		r_read = body + *length;
		return p_in.substr(body, *length);
	}
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

// The res:// path of a file: URI under the project's root and outside
// .godot/; empty for any other. Windows clients write drive letters in either
// case and percent-encode the colon; there and on macOS, the root's case may
// differ from the editor's.
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
	const std::string root = to_utf8(ProjectSettings::get_singleton()->globalize_path("res://"));
#if defined(_WIN32) || defined(__APPLE__)
	const bool under = lower_ascii(path.substr(0, root.size())) == lower_ascii(root);
#else
	const bool under = path.compare(0, root.size(), root) == 0;
#endif
	if (!under || path.size() <= root.size()) {
		return std::string();
	}
	const std::string relative = path.substr(root.size());
	for (size_t begin = 0; begin <= relative.size();) {
		const size_t end = std::min(relative.find('/', begin), relative.size());
		const std::string segment = relative.substr(begin, end - begin);
		if (segment.empty() || segment == "." || segment == ".." || (begin == 0 && segment == ".godot")) {
			return std::string();
		}
		begin = end + 1;
	}
	return "res://" + relative;
}

// Of a script of this language; empty for other files.
std::string script_path(const Variant &p_uri) {
	const std::string path = project_path(p_uri);
	return path.size() > 4 && path.compare(path.size() - 4, 4, ".pet") == 0 ? path : std::string();
}

} // namespace

LspServer::LspServer(Ref<EditorDebuggerPlugin> p_sessions) :
		sessions(std::move(p_sessions)) {
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
	const Ref<FileAccess> file = FileAccess::open(path, FileAccess::WRITE);
	if (file.is_valid()) {
		file->store_string(JSON::stringify(info));
	}
}

// The file stays if another editor has written it since.
LspServer::~LspServer() {
	if (client.peer.is_valid()) {
		client.peer->disconnect_from_host();
	}
	for (Connection &each : refused) {
		each.peer->disconnect_from_host();
	}
	server->stop();
	const String path = info_path();
	Ref<JSON> json;
	json.instantiate();
	if (FileAccess::file_exists(path) && json->parse(FileAccess::get_file_as_string(path)) == OK && integer(dictionary(json->get_data()).get("pid", Variant()), 0, INT32_MAX) == OS::get_singleton()->get_process_id()) {
		DirAccess::remove_absolute(path);
	}
}

// Sends what is due and reads what arrived; false once the connection is gone,
// or is closing and has sent everything.
bool LspServer::transfer(Connection &r_connection) {
	r_connection.peer->poll();
	if (r_connection.peer->get_status() != StreamPeerSocket::STATUS_CONNECTED) {
		return false;
	}
	if (!r_connection.out.empty()) {
		const size_t size = std::min(r_connection.out.size(), size_t(CHUNK));
		PackedByteArray bytes;
		bytes.resize(int64_t(size));
		std::memcpy(bytes.ptrw(), r_connection.out.data(), size);
		const Array sent = r_connection.peer->put_partial_data(bytes);
		if (int64_t(sent[0]) != OK) {
			return false;
		}
		r_connection.out.erase(0, size_t(int64_t(sent[1])));
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
	polling = false;
}

void LspServer::step() {
	while (server->is_connection_available()) {
		Connection connection;
		connection.peer = server->take_connection();
		if (client.peer.is_null()) {
			client = std::move(connection);
		} else if (refused.size() < MAX_REFUSED) {
			refused.push_back(std::move(connection));
		} else {
			connection.peer->disconnect_from_host();
		}
	}
	for (auto each = refused.begin(); each != refused.end();) {
		if (!transfer(*each)) {
			each->peer->disconnect_from_host();
			each = refused.erase(each);
			continue;
		}
		while (!each->closing) {
			const std::optional<std::string> body = next_message(each->in, each->read);
			if (!body) {
				break;
			}
			handle(*each, *body);
		}
		each->in.erase(0, each->read);
		each->read = 0;
		++each;
	}
	if (client.peer.is_null()) {
		return;
	}
	// Before reading, so that a text held since the last frame is offered at
	// the version of the text it was made from.
	if (state == State::Running) {
		for (auto &[path, document] : documents) {
			offer_held(path, document);
		}
	}
	if (!transfer(client)) {
		disconnect();
		return;
	}
	for (const uint64_t start = Time::get_singleton()->get_ticks_msec(); !client.closing && Time::get_singleton()->get_ticks_msec() - start < FRAME_BUDGET;) {
		const std::optional<std::string> body = next_message(client.in, client.read);
		if (!body) {
			break;
		}
		handle(client, *body);
	}
	client.in.erase(0, client.read);
	client.read = 0;
	if (state != State::Running || client.closing) {
		return;
	}
	// Once the workspace has stayed for as long as the script editor waits
	// before it checks a script, or the editor has taken in a change; a script
	// just opened at once (architecture §2.4).
	const uint64_t revision = workspace().revision();
	const uint64_t now = Time::get_singleton()->get_ticks_msec();
	if (revision != seen) {
		seen = revision;
		quiet_since = now;
	}
	const bool quiet = settle || now - quiet_since >= delay;
	settle = false;
	for (auto &[path, document] : documents) {
		if (document.published != revision && (quiet || document.published == 0)) {
			publish(path, document);
		}
	}
}

void LspServer::handle(Connection &r_connection, const std::string &p_body) {
	Ref<JSON> json;
	json.instantiate();
	if (!is_utf8(p_body) || json->parse(to_godot(p_body)) != OK) {
		reply_error(r_connection.out, Variant(), PARSE_ERROR, "The message is not JSON in UTF-8.");
		return;
	}
	const Variant data = json->get_data();
	const Dictionary message = dictionary(data);
	const Variant id = request_id(message.get("id", Variant()));
	const Variant method = message.get("method", Variant());
	const bool has_id = id.get_type() != Variant::NIL;
	if (data.get_type() != Variant::DICTIONARY || (message.has("id") && !has_id) || (!message.has("method") && !has_id) || (message.has("method") && method.get_type() != Variant::STRING)) {
		reply_error(r_connection.out, id, INVALID_REQUEST, "The message is no JSON-RPC request, notification or response.");
	} else if (&r_connection != &client) {
		if (has_id) {
			reply_error(r_connection.out, id, REQUEST_FAILED, "Another client is connected to the Petrichor language server of this Godot editor.");
			r_connection.closing = true;
		}
	} else if (!message.has("method")) {
		answered(id, message);
	} else if (has_id) {
		request(id, method, message.get("params", Variant()));
	} else {
		notification(method, dictionary(message.get("params", Variant())));
	}
}

// Before `initialize` and after `shutdown`, only `exit` is taken
// (LSP 3.17, Lifecycle Messages).
void LspServer::request(const Variant &p_id, const String &p_method, const Variant &p_params) {
	if (p_method == "initialize" && state == State::Waiting) {
		const Dictionary capabilities = dictionary(dictionary(p_params).get("capabilities", Variant()));
		applies_edits = dictionary(capabilities.get("workspace", Variant())).get("applyEdit", false) == Variant(true);
		Dictionary sync;
		sync["openClose"] = true;
		sync["change"] = 2; // Incremental.
		sync["save"] = true;
		Dictionary offered;
		offered["positionEncoding"] = "utf-16";
		offered["textDocumentSync"] = sync;
		Dictionary info;
		info["name"] = "Petrichor";
		Dictionary result;
		result["capabilities"] = offered;
		result["serverInfo"] = info;
		reply(client.out, p_id, result);
		state = State::Running;
	} else if (state == State::Waiting) {
		reply_error(client.out, p_id, SERVER_NOT_INITIALIZED, "The server is not initialized.");
	} else if (p_method == "initialize" || state == State::ShutDown) {
		reply_error(client.out, p_id, INVALID_REQUEST, state == State::ShutDown ? "The server is shut down." : "The server is initialized already.");
	} else if (p_method == "shutdown") {
		state = State::ShutDown;
		reply(client.out, p_id, Variant());
	} else {
		reply_error(client.out, p_id, METHOD_NOT_FOUND, "Unknown method: " + p_method);
	}
}

// One with a malformed field is ignored.
void LspServer::notification(const String &p_method, const Dictionary &p_params) {
	if (p_method == "exit") {
		client.closing = true;
		return;
	}
	if (state != State::Running) {
		return;
	}
	if (p_method == "textDocument/didOpen") {
		opened(p_params);
	} else if (p_method == "textDocument/didChange") {
		changed(p_params);
	} else if (p_method == "textDocument/didClose") {
		closed(p_params);
	} else if (p_method == "textDocument/didSave") {
		const std::string path = script_path(dictionary(p_params.get("textDocument", Variant())).get("uri", Variant()));
		if (!path.empty()) {
			saved(path);
		}
	} else if (p_method == "workspace/didChangeWatchedFiles") {
		// As the editor scans when it has focus again (editor/editor_node.cpp:1049-1058; ed1daf0).
		const Variant changes = p_params.get("changes", Variant());
		const Array list = changes.get_type() == Variant::ARRAY ? Array(changes) : Array();
		for (int64_t i = 0; i < list.size(); i++) {
			if (!project_path(dictionary(list[i]).get("uri", Variant())).empty()) {
				disk_changed();
				EditorInterface::get_singleton()->get_resource_filesystem()->scan();
				return;
			}
		}
	}
}

// The reply to workspace/applyEdit. Applied, the text is no longer held: it
// is the client's unsaved change.
void LspServer::answered(const Variant &p_id, const Dictionary &p_message) {
	const auto offer = p_id.get_type() == Variant::INT ? offers.find(int64_t(p_id)) : offers.end();
	if (offer == offers.end()) {
		return;
	}
	const auto [path, text] = offer->second;
	offers.erase(offer);
	const Dictionary result = dictionary(p_message.get("result", Variant()));
	if (result.get("applied", false) == Variant(true)) {
		const auto held = held_scripts().find(path);
		if (held != held_scripts().end() && held->second == text) {
			drop_held(path);
		}
		return;
	}
	const Variant reason = result.get("failureReason", dictionary(p_message.get("error", Variant())).get("message", Variant()));
	show_message(vformat("Godot holds unsaved changes to %s from a refactoring, which could not be applied here%s. Save them in Godot first.", to_godot(path), reason.get_type() == Variant::STRING ? ": " + String(reason) : String()));
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
	Document &document = documents[path];
	document = Document{ uri, *version };
	set_client_script(path, to_utf8(text));
	offer_held(path, document);
}

// Each change applies to the text that the one before it left (LSP 3.17,
// DidChangeTextDocumentParams).
void LspServer::changed(const Dictionary &p_params) {
	const Dictionary item = dictionary(p_params.get("textDocument", Variant()));
	const auto found = documents.find(script_path(item.get("uri", Variant())));
	const std::optional<int64_t> version = integer(item.get("version", Variant()), INT32_MIN, INT32_MAX);
	const Variant changes = p_params.get("contentChanges", Variant());
	if (found == documents.end() || !version || changes.get_type() != Variant::ARRAY) {
		return;
	}
	std::string text = workspace().script_source(found->first).value_or(std::string());
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
	found->second.version = *version;
	set_client_script(found->first, std::move(text));
}

// Back to the tab or the file, and its diagnostics cleared.
void LspServer::closed(const Dictionary &p_params) {
	const auto found = documents.find(script_path(dictionary(p_params.get("textDocument", Variant())).get("uri", Variant())));
	if (found == documents.end()) {
		return;
	}
	Dictionary params;
	params["uri"] = found->second.uri;
	params["diagnostics"] = Array();
	drop_client_script(found->first);
	documents.erase(found);
	notify("textDocument/publishDiagnostics", params);
}

// As the editor's own save of a script (architecture §2.4): the editor's file
// system takes the file in, and a loaded script that no tab has reads it again
// (editor/file_system/editor_file_system.cpp:2259-2271, 2292-2309; ed1daf0).
// Its bytecode is written, and a running game reloads it, as the script
// editor has a saved script without errors reloaded while Debug > Synchronize
// Script Changes is on (editor/script/script_editor_plugin.cpp:776-801,
// editor/debugger/debugger_editor_plugin.cpp:238; ed1daf0). Tabs are left to
// the editor, which reloads or asks once it has focus again (architecture §2.4).
void LspServer::saved(const std::string &p_path) {
	disk_changed();
	EditorInterface *editor = EditorInterface::get_singleton();
	const String path = to_godot(p_path);
	editor->get_resource_filesystem()->update_file(path);
	if (!FileAccess::file_exists(path)) {
		return;
	}
	const std::shared_ptr<const ClassImage> image = compile_script(path, FileAccess::get_file_as_string(path), nullptr, false);
	if (!image || !write_compiled(cache_path(path), *image) || editor->get_editor_settings()->get_project_metadata("debug_options", "run_reload_scripts", true) != Variant(true)) {
		return;
	}
	const Array each = sessions->get_sessions();
	for (int64_t i = 0; i < each.size(); i++) {
		const Ref<EditorDebuggerSession> session = each[i];
		if (session->is_active()) {
			session->send_message("reload_scripts", Array::make(path));
		}
	}
}

// A text that a refactoring in the editor holds for a script the client has
// open goes to the client as an unsaved change of its own (architecture §2.3,
// §2.4), at the version it was made from; once each.
void LspServer::offer_held(const std::string &p_path, Document &r_document) {
	const auto held = held_scripts().find(p_path);
	if (held == held_scripts().end() || held->second == r_document.offered) {
		return;
	}
	r_document.offered = held->second;
	if (!applies_edits) {
		show_message(vformat("Godot holds unsaved changes to %s from a refactoring, which this client cannot take. Save them in Godot first.", to_godot(p_path)));
		return;
	}
	const std::string text = workspace().script_source(p_path).value_or(std::string());
	Dictionary edit;
	edit["range"] = range({}, compiler::utf16_positions(text, { uint32_t(text.size()) })[0]);
	edit["newText"] = to_godot(held->second);
	Dictionary document;
	document["uri"] = r_document.uri;
	document["version"] = r_document.version;
	Dictionary change;
	change["textDocument"] = document;
	change["edits"] = Array::make(edit);
	Dictionary changes;
	changes["documentChanges"] = Array::make(change);
	Dictionary params;
	params["label"] = "Unsaved changes in Godot";
	params["edit"] = changes;
	Dictionary message;
	message["id"] = next_id;
	message["method"] = "workspace/applyEdit";
	message["params"] = params;
	offers[next_id++] = { p_path, held->second };
	send(client.out, message);
}

// The diagnostics that the script editor shows for the client's text: the
// script's own, with those of the scenes that place it (architecture §2.4).
void LspServer::publish(const std::string &p_path, Document &r_document) {
	const uint64_t revision = workspace().revision();
	const std::string text = workspace().script_source(p_path).value_or(std::string());
	std::vector<compiler::Diagnostic> found;
	{
		const compiler::Analysis::Pass pass(analysis());
		const std::shared_ptr<const compiler::CompileResult> result = analysis().compile(p_path, text);
		const std::vector<compiler::Diagnostic> placed = analysis().placed_errors(p_path);
		found = result->errors;
		found.insert(found.end(), placed.begin(), placed.end());
		found.insert(found.end(), result->warnings.begin(), result->warnings.end());
	}
	std::vector<uint32_t> offsets;
	for (const compiler::Diagnostic &diagnostic : found) {
		offsets.push_back(diagnostic.begin);
		offsets.push_back(std::max(diagnostic.begin, diagnostic.end));
	}
	const std::vector<compiler::Utf16Position> at = compiler::utf16_positions(text, offsets);
	Array diagnostics;
	for (size_t i = 0; i < found.size(); i++) {
		Dictionary each;
		each["range"] = range(at[2 * i], at[2 * i + 1]);
		each["severity"] = severity(found[i].severity);
		each["source"] = "petrichor";
		each["message"] = to_godot(found[i].message);
		if (!found[i].warning.empty()) {
			each["code"] = to_godot(found[i].warning);
		}
		diagnostics.push_back(each);
	}
	Dictionary params;
	params["uri"] = r_document.uri;
	params["version"] = r_document.version;
	params["diagnostics"] = diagnostics;
	notify("textDocument/publishDiagnostics", params);
	r_document.published = revision;
}

void LspServer::notify(const char *p_method, const Dictionary &p_params) {
	Dictionary message;
	message["method"] = p_method;
	message["params"] = p_params;
	send(client.out, message);
}

void LspServer::show_message(const String &p_message) {
	Dictionary params;
	params["type"] = 2; // Warning.
	params["message"] = p_message;
	notify("window/showMessage", params);
}

// The scripts the client had open go back to their tabs or files.
void LspServer::disconnect() {
	client.peer->disconnect_from_host();
	for (const auto &[path, document] : documents) {
		drop_client_script(path);
	}
	documents.clear();
	offers.clear();
	client = Connection();
	state = State::Waiting;
	applies_edits = false;
}

} // namespace petrichor
