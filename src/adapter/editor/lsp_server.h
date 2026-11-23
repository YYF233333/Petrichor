#pragma once

#include "adapter/editor/project_files.h"
#include "compiler/diagnostic.h"

#include <godot_cpp/classes/editor_debugger_plugin.hpp>
#include <godot_cpp/classes/stream_peer_tcp.hpp>
#include <godot_cpp/classes/tcp_server.hpp>
#include <godot_cpp/variant/dictionary.hpp>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <list>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace petrichor {

namespace compiler {
class ProjectApi;
}
struct Held;

// The language server in the editor (architecture §2.4): JSON-RPC with
// Content-Length headers over TCP on 127.0.0.1, at the port the system gives,
// which .godot/petrichor/lsp.json names with the editor's process id and the
// build (architecture §6.5). It
// serves each connection as a session of its own, on the main thread, polled
// each frame, and shares the workspace and the analysis with the editor.
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
		bool closing = false; // Once `out` is sent, after `exit`.

		// The body of the next message that has all arrived. `r_skipped` gets
		// the length of one over the limit, passed over.
		std::optional<std::string> next(size_t &r_skipped);
	};
	enum class State {
		Waiting, // For `initialize`.
		Running,
		ShutDown,
	};
	// A script a session has open, by path: the version of each spelling of
	// its URI it has it open under, as the client wrote it, edits naming the
	// first; and its text, which is the session's own.
	struct Document {
		std::map<godot::String, int64_t> versions;
		std::string text;
		uint64_t published = 0; // What revision() was when the diagnostics sent were computed.
	};
	struct Session {
		Connection connection;
		State state = State::Waiting;
		bool disk = false; // On the disk view, not the human's (architecture §6.5).
		bool snippets = false; // In completion items.
		bool resolves_edits = false; // Of code actions, when one is chosen.
		bool annotates = false; // Edits that need confirmation.
		std::map<std::string, Document> documents;
	};

public:
	// A command of pet with its arguments and the conflicts to warn of: its
	// output and exit code, nullopt for no command.
	using Run = std::function<std::optional<std::pair<std::string, int>>(const std::string &, const std::vector<std::string> &, const std::set<std::string> &)>;

private:
	godot::Ref<godot::TCPServer> server;
	godot::Ref<godot::EditorDebuggerPlugin> debugger;
	std::function<void()> editor_changed;
	std::function<void(const std::set<std::string> &)> editor_written;
	Run run;
	uint64_t told = 0; // The workspace's revision that editor_changed() was last called at.
	std::list<Session> sessions;
	Session *session = nullptr; // Whose message is handled, or whose diagnostics are sent.
	const compiler::ProjectApi *view = nullptr; // What it sees, while its request is answered.
	godot::String failure; // Why the query being answered failed.
	uint64_t edited = 0; // The sessions' documents, as often as they changed.
	uint64_t seen = 0; // revision(), unchanged since `quiet_since` (ms).
	uint64_t quiet_since = 0;
	uint64_t delay = 0; // In ms, as the script editor waits before it checks.
	bool settle = false;
	uint64_t continuing = 0; // revision() whose diagnostics the frame before did not all send.
	bool polling = false;
	// The project's files as last taken in, which a thread compares with the
	// disk (architecture §6.5).
	const std::string root = project_directory();
	std::map<std::string, FileStat> walked;
	std::mutex walking; // Around `walked` and `stopping`.
	std::condition_variable wake;
	bool stopping = false;
	std::atomic<bool> stale{ false }; // The thread found other files.
	std::thread walker;
	std::optional<std::map<std::string, FileStat>> saving; // Walked as the editor saved a scene.
	// The files that changed on disk while Godot holds unsaved changes of
	// them, as refresh() took them in (architecture §6.5).
	std::set<std::string> conflicts;

	// The workspace's revision with the sessions' edits, which their
	// diagnostics depend on.
	uint64_t revision() const;
	void step();
	bool flush(Connection &r_connection);
	bool transfer(Connection &r_connection);
	void handle(const std::string &p_body);
	void request(const godot::Variant &p_id, const godot::String &p_method, const godot::Variant &p_params);
	void notification(const godot::String &p_method, const godot::Dictionary &p_params);
	void opened(const godot::Dictionary &p_params);
	void changed(const godot::Dictionary &p_params);
	void closed(const godot::Dictionary &p_params);
	void release(const std::string &p_path);
	void watch(const std::string &p_root);
	void refresh();
	// The files Godot holds unsaved changes of: its unsaved tabs and scenes,
	// and the edited resources in conflict, as no binding lists the others.
	// A conflict on another file ends.
	std::set<std::string> held();
	void in_view(const std::function<void()> &p_do);
	void publish(const std::string &p_path, Document &r_document);
	void notify(const char *p_method, const godot::Dictionary &p_params);
	void show_message(const godot::String &p_message);
	std::list<Session>::iterator end(std::list<Session>::iterator p_session);

	godot::String uri_of(const std::string &p_path) const;
	godot::Dictionary location(const std::string &p_path, const std::string &p_text, uint32_t p_begin, uint32_t p_end) const;
	godot::Dictionary edit_of(const std::map<std::string, std::vector<compiler::CheckedEdit>> &p_edits, Held &r_held, const std::set<std::pair<std::string, uint32_t>> &p_confirm = {}) const;
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
	// `p_changed` is what the editor does once a change has settled: it checks
	// what it shows again (architecture §2.4). `p_written` is what it does once
	// a refactoring wrote files: it reloads what it has loaded of them. `p_run`
	// runs pet's commands (architecture §6.5).
	LspServer(godot::Ref<godot::EditorDebuggerPlugin> p_debugger, std::function<void()> p_changed, std::function<void(const std::set<std::string> &)> p_written, Run p_run);
	~LspServer();
	void poll();
	// The editor has taken in a change: what the sessions have open is checked
	// again without waiting.
	void settled() { settle = true; }
	// The editor saved a scene, and is to save the resources it edited, which
	// it does not report, until it saves the external data of its plugins
	// (architecture §2.4): what changes in between is its own writing.
	void scene_saving() { saving = walk_project(root); }
	void resources_saved();
	// Whether the file, which Godot holds unsaved changes of, changed on disk
	// since the editor last saw it, also before refresh() takes the change in.
	bool conflicts_with_disk(const std::string &p_path);
};

} // namespace petrichor
