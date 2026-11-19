#pragma once

#include "compiler/diagnostic.h"

#include <godot_cpp/classes/editor_debugger_plugin.hpp>
#include <godot_cpp/classes/stream_peer_tcp.hpp>
#include <godot_cpp/classes/tcp_server.hpp>
#include <godot_cpp/variant/dictionary.hpp>

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace petrichor {

// The language server in the editor (architecture §2.4): JSON-RPC with
// Content-Length headers over TCP on 127.0.0.1, at the port the system gives,
// which .godot/petrichor/lsp.json names with the editor's process id. It
// serves one client at a time on the main thread, polled each frame, and
// shares the workspace and the analysis with the editor.
class LspServer {
	// Bytes read and not yet handled from `read` on, and bytes not yet sent.
	struct Connection {
		godot::Ref<godot::StreamPeerTCP> peer;
		std::string in;
		size_t read = 0;
		size_t skip = 0; // Of a message over the limit, yet to arrive.
		bool seeking = false; // For a header, after one without a usable length.
		std::string out;
		uint64_t since = 0; // When it connected, in ms.
		bool closing = false; // Once `out` is sent. A connection refused is told so first.

		// The body of the next message that has all arrived. `r_skipped` gets
		// the length of one over the limit, passed over.
		std::optional<std::string> next(size_t &r_skipped);
	};
	enum class State {
		Waiting, // For `initialize`.
		Running,
		ShutDown,
	};
	// A script the client has open, by path: the version of each spelling of
	// its URI it has it open under, as the client wrote it; edits name the
	// first.
	struct Document {
		std::map<godot::String, int64_t> versions;
		uint64_t published = 0; // The workspace's revision that the diagnostics sent were computed at.
		std::string offered; // The text held for it that was last offered.
	};

	godot::Ref<godot::TCPServer> server;
	godot::Ref<godot::EditorDebuggerPlugin> sessions;
	Connection client;
	std::vector<Connection> refused; // Other clients, each told so on its first request.
	State state = State::Waiting;
	bool applies_edits = false;
	bool snippets = false; // In completion items.
	bool shows_disabled = false; // Code actions with a reason they are disabled.
	bool resolves_edits = false; // Of code actions, when one is chosen.
	bool annotates = false; // Edits that need confirmation.
	godot::String failure; // Why the query being answered failed.
	std::map<std::string, Document> documents;
	std::map<int64_t, std::pair<std::string, std::string>> offers; // The path and the text of each workspace/applyEdit, by id.
	int64_t next_id = 1;
	uint64_t seen = 0; // The workspace's revision, unchanged since `quiet_since` (ms).
	uint64_t quiet_since = 0;
	uint64_t delay = 0; // In ms, as the script editor waits before it checks.
	bool settle = false;
	uint64_t continuing = 0; // The revision whose diagnostics the frame before did not all send.
	bool polling = false;

	void step();
	bool flush(Connection &r_connection);
	bool transfer(Connection &r_connection);
	void handle(Connection &r_connection, const std::string &p_body);
	void request(const godot::Variant &p_id, const godot::String &p_method, const godot::Variant &p_params);
	void notification(const godot::String &p_method, const godot::Dictionary &p_params);
	void answered(const godot::Variant &p_id, const godot::Dictionary &p_message);
	void opened(const godot::Dictionary &p_params);
	void changed(const godot::Dictionary &p_params);
	void closed(const godot::Dictionary &p_params);
	void release(const std::string &p_path);
	void saved(const std::string &p_path);
	void files_changed();
	void offer_held(const std::string &p_path, Document &r_document);
	void publish(const std::string &p_path, Document &r_document);
	void notify(const char *p_method, const godot::Dictionary &p_params);
	void show_message(const godot::String &p_message);
	void disconnect();

	godot::String uri_of(const std::string &p_path) const;
	godot::Dictionary location(const std::string &p_path, const std::string &p_text, uint32_t p_begin, uint32_t p_end) const;
	godot::Dictionary edit_of(const std::map<std::string, std::vector<compiler::CheckedEdit>> &p_edits, godot::String &r_refused, const std::set<std::pair<std::string, uint32_t>> &p_confirm = {}) const;
	godot::Variant on_completion(const godot::Dictionary &p_params);
	godot::Variant on_signature_help(const godot::Dictionary &p_params);
	godot::Variant on_hover(const godot::Dictionary &p_params);
	godot::Variant on_definition(const godot::Dictionary &p_params);
	godot::Variant on_type_definition(const godot::Dictionary &p_params);
	godot::Variant on_references(const godot::Dictionary &p_params);
	godot::Variant on_document_symbol(const godot::Dictionary &p_params);
	godot::Variant on_highlight(const godot::Dictionary &p_params);
	godot::Variant on_formatting(const godot::Dictionary &p_params);
	godot::Variant on_code_action(const godot::Dictionary &p_params);
	godot::Variant on_resolve_action(const godot::Dictionary &p_params);
	godot::Variant on_prepare_rename(const godot::Dictionary &p_params);
	godot::Variant on_rename(const godot::Dictionary &p_params);

public:
	explicit LspServer(godot::Ref<godot::EditorDebuggerPlugin> p_sessions);
	~LspServer();
	void poll();
	// The editor has taken in a change: what the client has open is checked
	// again without waiting.
	void settled() { settle = true; }
};

} // namespace petrichor
