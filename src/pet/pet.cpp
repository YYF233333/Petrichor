// pet, the command line's client of the project service (architecture §6.5).
// It runs check, build, format and migrate in the Godot editor that the project's
// .godot/petrichor/lsp.json names, else in a headless editor, and bridges a
// language client on stdio to the service. It does not depend on Godot.

#include "build_id.gen.h"
#include "compiler/lexer.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <winsock2.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <climits>
#include <csignal>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
extern char **environ;
#endif

namespace {

// The exit codes.
constexpr int NO_ERRORS = 0;
constexpr int ERRORS = 1;
constexpr int NOT_RUN = 2;

const char *const USAGE =
		"Usage: pet <command> [--godot <path>]\n"
		"\n"
		"  check             checks the project on disk, writing nothing\n"
		"  build             checks it and writes the bytecode\n"
		"  format [--check]  formats the scripts, or lists those not formatted\n"
		"  migrate [--dry-run] [<path>...]\n"
		"                    migrates the GDScript files under the paths, from the\n"
		"                    project's root, or all; with --dry-run, only reports\n"
		"  lsp               bridges a language client on stdio to the service\n"
		"\n"
		"The command runs in the Godot editor open on the project, else in a\n"
		"headless one. --godot names its executable where no editor has\n"
		"recorded one. Exit codes: 0 no errors, 1 errors, 2 not run; migrate\n"
		"gives 1 only when it wrote nothing.\n";

// Platform layer: text and files, processes, sockets.

#ifdef _WIN32
std::wstring wide(const std::string &p_text) {
	std::wstring result(MultiByteToWideChar(CP_UTF8, 0, p_text.data(), int(p_text.size()), nullptr, 0), L'\0');
	MultiByteToWideChar(CP_UTF8, 0, p_text.data(), int(p_text.size()), result.data(), int(result.size()));
	return result;
}

std::string narrow(const std::wstring &p_text) {
	std::string result(WideCharToMultiByte(CP_UTF8, 0, p_text.data(), int(p_text.size()), nullptr, 0, nullptr, nullptr), '\0');
	WideCharToMultiByte(CP_UTF8, 0, p_text.data(), int(p_text.size()), result.data(), int(result.size()), nullptr, nullptr);
	return result;
}
#endif

// To standard output (1) or error (2); false once it is closed.
bool write_bytes(int p_stream, const char *p_data, size_t p_size) {
	while (p_size > 0) {
#ifdef _WIN32
		DWORD written = 0;
		if (!WriteFile(GetStdHandle(p_stream == 1 ? STD_OUTPUT_HANDLE : STD_ERROR_HANDLE), p_data, DWORD(std::min(p_size, size_t(1) << 20)), &written, nullptr)) {
			return false;
		}
#else
		const ssize_t written = write(p_stream, p_data, p_size);
		if (written < 0 && errno == EINTR) {
			continue;
		}
		if (written < 0) {
			return false;
		}
#endif
		p_data += written;
		p_size -= size_t(written);
	}
	return true;
}

// UTF-8 text, which a Windows console takes as UTF-16.
void print(int p_stream, const std::string &p_text) {
#ifdef _WIN32
	const HANDLE handle = GetStdHandle(p_stream == 1 ? STD_OUTPUT_HANDLE : STD_ERROR_HANDLE);
	DWORD mode = 0;
	if (GetConsoleMode(handle, &mode)) {
		const std::wstring text = wide(p_text);
		DWORD written = 0;
		WriteConsoleW(handle, text.data(), DWORD(text.size()), &written, nullptr);
		return;
	}
#endif
	write_bytes(p_stream, p_text.data(), p_text.size());
}

// What the standard input gives next; 0 once it ends.
size_t read_input(char *r_buffer, size_t p_size) {
#ifdef _WIN32
	DWORD read = 0;
	return ReadFile(GetStdHandle(STD_INPUT_HANDLE), r_buffer, DWORD(p_size), &read, nullptr) ? size_t(read) : 0;
#else
	ssize_t read = 0;
	do {
		read = ::read(0, r_buffer, p_size);
	} while (read < 0 && errno == EINTR);
	return read > 0 ? size_t(read) : 0;
#endif
}

std::optional<std::string> read_file(const std::string &p_path) {
#ifdef _WIN32
	FILE *file = _wfopen(wide(p_path).c_str(), L"rb");
#else
	FILE *file = std::fopen(p_path.c_str(), "rb");
#endif
	if (!file) {
		return std::nullopt;
	}
	std::string data;
	char buffer[4096];
	for (size_t read; (read = std::fread(buffer, 1, sizeof(buffer), file)) > 0;) {
		data.append(buffer, read);
	}
	std::fclose(file);
	return data;
}

bool exists(const std::string &p_path) {
#ifdef _WIN32
	return GetFileAttributesW(wide(p_path).c_str()) != INVALID_FILE_ATTRIBUTES;
#else
	return access(p_path.c_str(), F_OK) == 0;
#endif
}

void remove_file(const std::string &p_path) {
#ifdef _WIN32
	DeleteFileW(wide(p_path).c_str());
#else
	unlink(p_path.c_str());
#endif
}

// Empty, with the directories it is in.
void empty_file(const std::string &p_path) {
	for (size_t slash = p_path.find('/', 1); slash != std::string::npos; slash = p_path.find('/', slash + 1)) {
#ifdef _WIN32
		CreateDirectoryW(wide(p_path.substr(0, slash)).c_str(), nullptr);
#else
		mkdir(p_path.substr(0, slash).c_str(), 0755);
#endif
	}
#ifdef _WIN32
	FILE *file = _wfopen(wide(p_path).c_str(), L"wb");
#else
	FILE *file = std::fopen(p_path.c_str(), "wb");
#endif
	if (file) {
		std::fclose(file);
	}
}

std::string executable_path() {
#ifdef _WIN32
	std::wstring path(32768, L'\0');
	path.resize(GetModuleFileNameW(nullptr, path.data(), DWORD(path.size())));
	std::string result = narrow(path);
	std::replace(result.begin(), result.end(), '\\', '/');
	return result;
#elif defined(__APPLE__)
	uint32_t size = 0;
	_NSGetExecutablePath(nullptr, &size);
	std::string path(size, '\0');
	_NSGetExecutablePath(path.data(), &size);
	char resolved[PATH_MAX];
	return realpath(path.c_str(), resolved) ? std::string(resolved) : std::string(path.c_str());
#else
	char path[PATH_MAX];
	const ssize_t size = readlink("/proc/self/exe", path, sizeof(path));
	return size > 0 ? std::string(path, size_t(size)) : std::string();
#endif
}

bool alive(long p_pid) {
#ifdef _WIN32
	const HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, DWORD(p_pid));
	if (!process) {
		return GetLastError() == ERROR_ACCESS_DENIED;
	}
	DWORD code = 0;
	const bool running = GetExitCodeProcess(process, &code) && code == STILL_ACTIVE;
	CloseHandle(process);
	return running;
#else
	return p_pid > 0 && (kill(pid_t(p_pid), 0) == 0 || errno == EPERM);
#endif
}

// The one process pet started that is running, stopped as pet ends.
#ifdef _WIN32
HANDLE child = nullptr;
#else
pid_t child = -1;
#endif

void stop_child() {
#ifdef _WIN32
	if (child) {
		TerminateProcess(child, 1);
	}
#else
	if (child > 0) {
		kill(child, SIGTERM);
	}
#endif
}

[[noreturn]] void fail(const std::string &p_message) {
	stop_child();
	print(2, "pet: " + p_message + "\n");
	std::exit(NOT_RUN);
}

#ifdef _WIN32
// As the C runtime splits a command line into arguments.
std::wstring quoted(const std::wstring &p_argument) {
	if (!p_argument.empty() && p_argument.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
		return p_argument;
	}
	std::wstring result = L"\"";
	size_t backslashes = 0;
	for (const wchar_t c : p_argument) {
		if (c == L'\\') {
			backslashes++;
			continue;
		}
		result.append(c == L'"' ? backslashes * 2 + 1 : backslashes, L'\\');
		result += c;
		backslashes = 0;
	}
	return result.append(backslashes * 2, L'\\') + L"\"";
}
#endif

// Starts the command with nothing on its standard input. All its output goes
// to the log, so that pet prints only the command's text, the LSP messages on
// pet's standard output stay apart, and a client that reads nothing of pet's
// standard error does not stop the editor.
void start(const std::vector<std::string> &p_command, const std::string &p_log) {
#ifdef _WIN32
	std::wstring line;
	for (const std::string &argument : p_command) {
		line += (line.empty() ? L"" : L" ") + quoted(wide(argument));
	}
	SECURITY_ATTRIBUTES inherited = { sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE };
	const HANDLE nothing = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherited, OPEN_EXISTING, 0, nullptr);
	const HANDLE log = CreateFileW(wide(p_log).c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherited, OPEN_ALWAYS, 0, nullptr);
	STARTUPINFOW info = {};
	info.cb = sizeof(info);
	info.dwFlags = STARTF_USESTDHANDLES;
	info.hStdInput = nothing;
	info.hStdOutput = log;
	info.hStdError = log;
	// In a job that ends with pet, so that no editor outlives it. The
	// console wrapper of the engine's Windows builds starts the editor at once,
	// so the child is put in the job before it runs.
	static const HANDLE job = [] {
		const HANDLE made = CreateJobObjectW(nullptr, nullptr);
		JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {};
		limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
		SetInformationJobObject(made, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
		return made;
	}();
	PROCESS_INFORMATION started = {};
	const BOOL ok = CreateProcessW(nullptr, line.data(), nullptr, nullptr, TRUE, CREATE_SUSPENDED, nullptr, nullptr, &info, &started);
	CloseHandle(nothing);
	CloseHandle(log);
	if (!ok) {
		fail("cannot run " + p_command[0] + ".");
	}
	AssignProcessToJobObject(job, started.hProcess);
	ResumeThread(started.hThread);
	CloseHandle(started.hThread);
	child = started.hProcess;
#else
	posix_spawn_file_actions_t actions;
	posix_spawn_file_actions_init(&actions);
	posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0);
	posix_spawn_file_actions_addopen(&actions, 1, p_log.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
	posix_spawn_file_actions_adddup2(&actions, 1, 2);
	std::vector<char *> arguments;
	for (const std::string &argument : p_command) {
		arguments.push_back(const_cast<char *>(argument.c_str()));
	}
	arguments.push_back(nullptr);
	const int error = posix_spawnp(&child, arguments[0], &actions, nullptr, arguments.data(), environ);
	posix_spawn_file_actions_destroy(&actions);
	if (error != 0) {
		child = -1;
		fail("cannot run " + p_command[0] + ".");
	}
#endif
}

// The child's exit code once it has ended, -1 for a crash; nullopt while it
// runs unless `p_wait`.
std::optional<int> ended(bool p_wait) {
#ifdef _WIN32
	if (WaitForSingleObject(child, p_wait ? INFINITE : 0) != WAIT_OBJECT_0) {
		return std::nullopt;
	}
	DWORD code = 0;
	GetExitCodeProcess(child, &code);
	CloseHandle(child);
	child = nullptr;
	return code <= 255 ? int(code) : -1;
#else
	int status = 0;
	pid_t found = 0;
	do {
		found = waitpid(child, &status, p_wait ? 0 : WNOHANG);
	} while (found < 0 && errno == EINTR);
	if (found == 0) {
		return std::nullopt;
	}
	child = -1;
	return found > 0 && WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
}

#ifdef _WIN32
using Socket = SOCKET;
const Socket NO_SOCKET = INVALID_SOCKET;
#else
using Socket = int;
const Socket NO_SOCKET = -1;
#endif

Socket connect_to(long p_port) {
#ifdef _WIN32
	static const bool started = [] {
		WSADATA data;
		return WSAStartup(MAKEWORD(2, 2), &data) == 0;
	}();
	(void)started;
#endif
	const Socket made = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	sockaddr_in address = {};
	address.sin_family = AF_INET;
	address.sin_port = htons(uint16_t(p_port));
	address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (made != NO_SOCKET && connect(made, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) != 0) {
#ifdef _WIN32
		closesocket(made);
#else
		close(made);
#endif
		return NO_SOCKET;
	}
	return made;
}

bool send_all(Socket p_socket, const char *p_data, size_t p_size) {
	while (p_size > 0) {
		const int sent = send(p_socket, p_data, int(std::min(p_size, size_t(1) << 20)), 0);
		if (sent <= 0) {
			return false;
		}
		p_data += sent;
		p_size -= size_t(sent);
	}
	return true;
}

// JSON, as much as the service's messages and lsp.json need.

size_t skip_blanks(const std::string &p_json, size_t p_at) {
	while (p_at < p_json.size() && std::strchr(" \t\r\n", p_json[p_at])) {
		p_at++;
	}
	return p_at;
}

// Where the value that begins at `p_at` ends; npos where none does.
size_t value_end(const std::string &p_json, size_t p_at) {
	p_at = skip_blanks(p_json, p_at);
	if (p_at >= p_json.size()) {
		return std::string::npos;
	}
	if (p_json[p_at] == '"') {
		for (p_at++; p_at < p_json.size(); p_at++) {
			if (p_json[p_at] == '\\') {
				p_at++;
			} else if (p_json[p_at] == '"') {
				return p_at + 1;
			}
		}
		return std::string::npos;
	}
	if (p_json[p_at] != '{' && p_json[p_at] != '[') {
		const size_t end = p_json.find_first_of(",:]} \t\r\n", p_at);
		return end == std::string::npos ? p_json.size() : end;
	}
	const char close = p_json[p_at] == '{' ? '}' : ']';
	p_at = skip_blanks(p_json, p_at + 1);
	if (p_at < p_json.size() && p_json[p_at] == close) {
		return p_at + 1;
	}
	while (true) {
		if (close == '}') {
			p_at = skip_blanks(p_json, value_end(p_json, p_at));
			if (p_at >= p_json.size() || p_json[p_at] != ':') {
				return std::string::npos;
			}
			p_at++;
		}
		p_at = skip_blanks(p_json, value_end(p_json, p_at));
		if (p_at >= p_json.size() || (p_json[p_at] != ',' && p_json[p_at] != close)) {
			return std::string::npos;
		}
		if (p_json[p_at++] == close) {
			return p_at;
		}
	}
}

uint32_t hex4(const std::string &p_json, size_t p_at) {
	const std::string digits = p_json.substr(p_at, 4);
	return digits.size() == 4 && digits.find_first_not_of("0123456789abcdefABCDEF") == std::string::npos ? uint32_t(std::strtoul(digits.c_str(), nullptr, 16)) : 0xFFFD;
}

// The text of a string value; nullopt for another value.
std::optional<std::string> string_of(const std::optional<std::string> &p_json) {
	if (!p_json || p_json->size() < 2 || (*p_json)[0] != '"') {
		return std::nullopt;
	}
	const std::string &json = *p_json;
	std::string text;
	for (size_t at = 1; at + 1 < json.size(); at++) {
		if (json[at] != '\\') {
			text += json[at];
			continue;
		}
		const char escaped = json[++at];
		const char *const from = "bfnrt";
		const char *const to = "\b\f\n\r\t";
		if (const char *found = std::strchr(from, escaped); found && escaped) {
			text += to[found - from];
		} else if (escaped != 'u') {
			text += escaped;
		} else {
			uint32_t code = hex4(json, at + 1);
			at += 4;
			const uint32_t low = code >= 0xD800 && code < 0xDC00 && json.compare(at + 1, 2, "\\u") == 0 ? hex4(json, at + 3) : 0;
			if (low >= 0xDC00 && low < 0xE000) {
				code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
				at += 6;
			}
			petrichor::compiler::append_utf8(text, code);
		}
	}
	return text;
}

long number_of(const std::optional<std::string> &p_json) {
	return p_json ? std::strtol(p_json->c_str(), nullptr, 10) : -1;
}

// The member's value, as JSON; nullopt where the object has none.
std::optional<std::string> member(const std::string &p_object, const char *p_name) {
	size_t at = skip_blanks(p_object, 0);
	if (at >= p_object.size() || p_object[at] != '{') {
		return std::nullopt;
	}
	for (at++;;) {
		const size_t key_end = value_end(p_object, at);
		const size_t value = skip_blanks(p_object, key_end);
		if (value >= p_object.size() || p_object[value] != ':') {
			return std::nullopt;
		}
		const size_t begin = skip_blanks(p_object, value + 1);
		const size_t end = value_end(p_object, begin);
		if (end == std::string::npos) {
			return std::nullopt;
		}
		if (string_of(p_object.substr(skip_blanks(p_object, at), key_end - skip_blanks(p_object, at))) == p_name) {
			return p_object.substr(begin, end - begin);
		}
		at = skip_blanks(p_object, end) + 1;
	}
}

std::string json_string(const std::string &p_text) {
	std::string json = "\"";
	for (const char c : p_text) {
		if (c == '"' || c == '\\') {
			json += '\\';
			json += c;
		} else if (static_cast<unsigned char>(c) < 0x20) {
			char escaped[8];
			std::snprintf(escaped, sizeof(escaped), "\\u%04x", unsigned(c));
			json += escaped;
		} else {
			json += c;
		}
	}
	return json + "\"";
}

// The service.

struct Connection {
	Socket socket = NO_SOCKET;
	std::string in;

	bool send_message(const std::string &p_body) {
		const std::string message = "Content-Length: " + std::to_string(p_body.size()) + "\r\n\r\n" + p_body;
		return send_all(socket, message.data(), message.size());
	}

	// The body of the next message; nullopt once the service closes.
	std::optional<std::string> next() {
		while (true) {
			const size_t end = in.find("\r\n\r\n");
			const size_t length_at = in.find("Content-Length:");
			if (end != std::string::npos && length_at < end) {
				const size_t length = size_t(std::strtoul(in.c_str() + length_at + 15, nullptr, 10));
				if (in.size() >= end + 4 + length) {
					std::string body = in.substr(end + 4, length);
					in.erase(0, end + 4 + length);
					return body;
				}
			}
			char buffer[65536];
			const int received = recv(socket, buffer, int(sizeof(buffer)), 0);
			if (received <= 0) {
				return std::nullopt;
			}
			in.append(buffer, size_t(received));
		}
	}

	// The response's result; a refusal ends pet.
	std::string request(int p_id, const char *p_method, const std::string &p_params) {
		send_message("{\"jsonrpc\":\"2.0\",\"id\":" + std::to_string(p_id) + ",\"method\":\"" + p_method + "\",\"params\":" + p_params + "}");
		while (const std::optional<std::string> body = next()) {
			if (member(*body, "id") != std::to_string(p_id)) {
				continue;
			}
			if (const std::optional<std::string> error = member(*body, "error")) {
				fail(std::string("the Godot editor refused ") + p_method + ": " + string_of(member(*error, "message")).value_or(*error));
			}
			return member(*body, "result").value_or("null");
		}
		fail("the Godot editor closed the connection.");
	}
};

// The service that lsp.json names: its process runs and takes a connection.
// One of another build ends pet.
std::optional<Connection> connect_service(const std::string &p_info) {
	const std::optional<std::string> info = read_file(p_info);
	const long pid = info ? number_of(member(*info, "pid")) : -1;
	if (!info || !alive(pid)) {
		return std::nullopt;
	}
	Connection connection;
	connection.socket = connect_to(number_of(member(*info, "port")));
	if (connection.socket == NO_SOCKET) {
		return std::nullopt;
	}
	const std::string build = string_of(member(*info, "build")).value_or("unknown");
	if (build != PETRICHOR_BUILD_ID) {
		fail("this pet is of build " PETRICHOR_BUILD_ID ", but the Godot editor open on the project (process " + std::to_string(pid) + ") runs Petrichor of build " + build + ". Use the pet of the addon that editor loaded: after replacing addons/petrichor/, restart the editor.");
	}
	return connection;
}

std::string find_godot(const std::string &p_root, const std::string &p_given) {
	if (!p_given.empty()) {
		return p_given;
	}
	const std::optional<std::string> recorded = read_file(p_root + "/.godot/petrichor/editor_path");
	if (!recorded || recorded->empty()) {
		fail("no Godot editor is open on the project, and none has recorded its executable here. Name one with --godot <path>.");
	}
	if (!exists(*recorded)) {
		fail("the Godot editor last open on the project is no longer at " + *recorded + ". Name one with --godot <path>.");
	}
	return *recorded;
}

// Empties the log that the headless editor writes to. A fresh copy, without
// .godot/, is imported first. That run can crash as it exits (godot#111048),
// so its exit code is not checked.
void open_project(const std::string &p_root, const std::string &p_godot, const std::string &p_log) {
	const bool fresh = !exists(p_root + "/.godot");
	empty_file(p_log);
	if (fresh) {
		start({ p_godot, "--headless", "--path", p_root, "--import" }, p_log);
		ended(true);
	}
}

// The same from the service and from a headless editor.
int report(const std::string &p_output, long p_code) {
	print(1, p_output.empty() || p_output.back() == '\n' ? p_output : p_output + "\n");
	return p_code == NO_ERRORS || p_code == ERRORS ? int(p_code) : NOT_RUN;
}

int run_in_service(Connection &r_service, const std::string &p_command, const std::vector<std::string> &p_args) {
	r_service.request(1, "initialize", "{\"processId\":null,\"capabilities\":{},\"initializationOptions\":{\"view\":\"disk\"}}");
	std::string args;
	for (const std::string &arg : p_args) {
		args += (args.empty() ? "" : ",") + json_string(arg);
	}
	const std::string result = r_service.request(2, "petrichor/run", "{\"command\":" + json_string(p_command) + ",\"args\":[" + args + "]}");
	return report(string_of(member(result, "output")).value_or(std::string()), number_of(member(result, "exitCode")));
}

// The command's exit code and text come in a file, apart from what the engine
// prints: the code on the first line. The editor renames the file into place
// once it is whole, so a result there is complete even when the editor
// crashes as it exits (godot#111048). An editor that gives none is shown by
// the end of its log.
int run_headless(const std::string &p_root, const std::string &p_godot, const std::string &p_command, const std::vector<std::string> &p_args) {
	const std::string log = p_root + "/.godot/petrichor/pet.log";
	const std::string result = p_root + "/.godot/petrichor/pet.out";
	open_project(p_root, p_godot, log);
	remove_file(result);
	std::vector<std::string> command = { p_godot, "--headless", "-e", "--path", p_root, "--", "--petrichor-" + p_command, "--petrichor-output=" + result };
	command.insert(command.end(), p_args.begin(), p_args.end());
	start(command, log);
	const int code = ended(true).value_or(-1);
	const std::string output = read_file(result).value_or(std::string());
	remove_file(result);
	const size_t newline = output.find('\n');
	if (newline == std::string::npos) {
		const std::string text = read_file(log).value_or(std::string());
		size_t from = text.size();
		for (int i = 0; i <= 20 && from > 0; i++) {
			const size_t newline = text.rfind('\n', from - 1);
			from = newline == std::string::npos ? 0 : newline;
		}
		fail("the headless Godot editor ended with code " + std::to_string(code) + " and no result. The end of .godot/petrichor/pet.log:\n" + text.substr(from == 0 ? 0 : from + 1));
	}
	return report(output.substr(newline + 1), std::strtol(output.c_str(), nullptr, 10));
}

[[noreturn]] void finish() {
	static std::atomic<bool> finishing{ false };
	if (finishing.exchange(true)) {
		while (true) {
			std::this_thread::sleep_for(std::chrono::seconds(1));
		}
	}
	stop_child();
	std::_Exit(NO_ERRORS);
}

// Bytes both ways, until the client closes standard input or the service
// the connection. Without a service, a headless editor serves the client.
[[noreturn]] void bridge(std::optional<Connection> p_service, const std::string &p_root, const std::string &p_godot, const std::string &p_info) {
	if (!p_service) {
		const std::string godot = find_godot(p_root, p_godot);
		const std::string log = p_root + "/.godot/petrichor/lsp.log";
		open_project(p_root, godot, log);
		remove_file(p_info);
		start({ godot, "--headless", "-e", "--path", p_root, "--", "--petrichor-lsp" }, log);
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(2);
		while (!(p_service = connect_service(p_info))) {
			if (ended(false) || std::chrono::steady_clock::now() > deadline) {
				fail("the headless Godot editor did not start its language server. Its output is in .godot/petrichor/lsp.log.");
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(100));
		}
	}
	const Socket socket = p_service->socket;
	std::thread([socket] {
		char buffer[65536];
		for (size_t read; (read = read_input(buffer, sizeof(buffer))) > 0;) {
			if (!send_all(socket, buffer, read)) {
				break;
			}
		}
		finish();
	}).detach();
	char buffer[65536];
	for (int received; (received = recv(socket, buffer, int(sizeof(buffer)), 0)) > 0;) {
		if (!write_bytes(1, buffer, size_t(received))) {
			break;
		}
	}
	finish();
}

int run(const std::vector<std::string> &p_args) {
	if (p_args.empty() || p_args[0] == "--help" || p_args[0] == "-h") {
		print(p_args.empty() ? 2 : 1, USAGE);
		return p_args.empty() ? NOT_RUN : NO_ERRORS;
	}
	const std::string &command = p_args[0];
	std::string godot;
	std::vector<std::string> passed;
	for (size_t i = 1; i < p_args.size(); i++) {
		if (p_args[i] == "--godot" && i + 1 < p_args.size()) {
			godot = p_args[++i];
		} else if ((command == "format" && p_args[i] == "--check") || (command == "migrate" && (p_args[i] == "--dry-run" || p_args[i].rfind("-", 0) != 0))) {
			passed.push_back(p_args[i]);
		} else {
			fail("unknown argument " + p_args[i] + ".\n\n" + USAGE);
		}
	}
	if (command != "check" && command != "build" && command != "format" && command != "migrate" && command != "lsp") {
		fail("unknown command " + command + ".\n\n" + USAGE);
	}
	// This copy is the project's own: <root>/addons/petrichor/bin/pet.
	std::string root = executable_path();
	for (int i = 0; i < 4; i++) {
		root.erase(std::min(root.size(), root.find_last_of('/')));
	}
	if (!exists(root + "/project.godot")) {
		fail("pet runs from a project's addons/petrichor/bin/, which " + executable_path() + " is not in.");
	}
	const std::string info = root + "/.godot/petrichor/lsp.json";
	std::optional<Connection> service = connect_service(info);
	if (command == "lsp") {
		bridge(std::move(service), root, godot, info);
	}
	return service ? run_in_service(*service, command, passed) : run_headless(root, find_godot(root, godot), command, passed);
}

#ifndef _WIN32
void stopped(int p_signal) {
	stop_child();
	_exit(128 + p_signal);
}
#endif

} // namespace

#ifdef _WIN32
int wmain(int p_argc, wchar_t **p_argv) {
	std::vector<std::string> args;
	for (int i = 1; i < p_argc; i++) {
		args.push_back(narrow(p_argv[i]));
	}
	return run(args);
}
#else
int main(int p_argc, char **p_argv) {
	std::signal(SIGPIPE, SIG_IGN);
	for (const int each : { SIGINT, SIGTERM, SIGHUP }) {
		std::signal(each, stopped);
	}
	return run(std::vector<std::string>(p_argv + 1, p_argv + p_argc));
}
#endif
