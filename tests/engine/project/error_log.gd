@tool
extends Logger

# The errors the engine reports, for checks to compare with the ones they
# expect: as "file:line: message" for those of scripts in the project, and as
# the message alone for the engine's own and those of scripts without a path.

var _mutex := Mutex.new()
var _reported: Array[Dictionary] = []


func _log_error(_function: String, file: String, line: int, code: String, rationale: String, _editor_notify: bool, error_type: int, _script_backtraces: Array[ScriptBacktrace]) -> void:
	if error_type != ERROR_TYPE_ERROR and error_type != ERROR_TYPE_SCRIPT:
		return
	var text := code if rationale.is_empty() else rationale
	var in_project := error_type == ERROR_TYPE_SCRIPT and file.begins_with("res://")
	_mutex.lock()
	_reported.append({ "where": "%s:%d: %s" % [file.get_file(), line, text] if in_project else text, "text": text })
	_mutex.unlock()


# Compares the errors reported since the last call with `expected`. When they
# are the same, prints each as "<prefix>: expected error: <message>", which
# run.py accepts, and returns ""; otherwise returns what was reported.
func compare(expected: Array[String], prefix: String) -> String:
	_mutex.lock()
	var reported := _reported.duplicate()
	_reported.clear()
	_mutex.unlock()
	var where := reported.map(func(error): return error.where)
	if where != expected:
		return "reported %s" % [where]
	for error in reported:
		print(prefix, ": expected error: ", error.text)
	return ""
