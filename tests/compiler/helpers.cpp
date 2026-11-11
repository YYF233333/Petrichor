#include "helpers.h"

#include "compiler/analysis.h"
#include "compiler/script_classes.h"
#include "conventions.h"
#include "fake_engine.h"

#include <set>

namespace petrichor::tests {

const compiler::EngineApi &fake_engine() {
	static const FakeEngine engine;
	return engine;
}

compiler::CompileResult compile(const std::string &p_source) {
	check_syntax_conventions(p_source);
	check_formatting(p_source);
	compiler::CompileResult result = compiler::compile(p_source, fake_engine());
	if (result.ok()) {
		check_references(p_source, fake_engine(), compiler::CompileOptions());
		check_recovery(p_source, [](const std::string &p_cut) { compiler::compile(p_cut, fake_engine()); });
	}
	return result;
}

void FakeProject::add_script(const std::string &p_path, const std::string &p_source, const std::string &p_class_name) {
	sources[p_path] = p_source;
	if (!p_class_name.empty()) {
		global_classes[p_class_name] = p_path;
	}
}

bool FakeProject::extends_script(const std::string &p_script, const std::string &p_base) const {
	compiler::ScriptClasses classes(fake_engine(), this);
	std::set<std::string> seen;
	for (std::string path = p_script; !path.empty() && seen.insert(path).second;) {
		if (path == p_base) {
			return true;
		}
		const auto *header = classes.header(path);
		path = header ? classes.base_path(*header, path) : std::string();
	}
	return false;
}

std::vector<compiler::Scene> FakeProject::scenes_with(bool (*p_uses)(const FakeProject &, const compiler::SceneNode &, const std::string &), const std::string &p_script_path) const {
	std::vector<compiler::Scene> result;
	for (const compiler::Scene &scene : scenes) {
		if (std::any_of(scene.nodes.begin(), scene.nodes.end(), [&](const compiler::SceneNode &p_node) { return p_uses(*this, p_node, p_script_path); })) {
			result.push_back(scene);
		}
	}
	return result;
}

std::vector<compiler::Scene> FakeProject::scenes_with_script(const std::string &p_script_path) const {
	return scenes_with([](const FakeProject &p_project, const compiler::SceneNode &p_node, const std::string &p_script) {
		return p_project.extends_script(p_node.script, p_script);
	},
			p_script_path);
}

std::vector<compiler::Scene> FakeProject::scenes_instancing_script(const std::string &p_script_path) const {
	return scenes_with([](const FakeProject &p_project, const compiler::SceneNode &p_node, const std::string &p_script) {
		return p_node.is_instance && p_node.script.empty() && p_project.extends_script(p_node.instance_script, p_script);
	},
			p_script_path);
}

std::vector<compiler::Scene> FakeProject::scenes_instancing(const std::string &p_scene_path) const {
	std::vector<compiler::Scene> result;
	for (const compiler::Scene &scene : scenes) {
		if (scene.base == p_scene_path || std::any_of(scene.nodes.begin(), scene.nodes.end(), [&](const compiler::SceneNode &p_node) { return p_node.instance == p_scene_path; })) {
			result.push_back(scene);
		}
	}
	return result;
}

std::optional<compiler::Scene> FakeProject::scene(const std::string &p_path) const {
	for (const compiler::Scene &scene : scenes) {
		if (scene.path == p_path) {
			return scene;
		}
	}
	return std::nullopt;
}

std::optional<compiler::ResourceInfo> FakeProject::resource(const std::string &p_path) const {
	const auto found = resources.find(p_path);
	return found == resources.end() ? std::nullopt : std::optional<compiler::ResourceInfo>(found->second);
}

std::string FakeProject::uid_path(const std::string &p_uid) const {
	const auto found = uids.find(p_uid);
	return found == uids.end() ? std::string() : found->second;
}

std::string FakeProject::global_class_path(const std::string &p_name) const {
	const auto found = global_classes.find(p_name);
	return found == global_classes.end() ? std::string() : found->second;
}

std::string FakeProject::autoload_path(const std::string &p_name) const {
	const auto found = autoloads.find(p_name);
	return found == autoloads.end() ? std::string() : found->second;
}

std::vector<std::string> FakeProject::autoload_names() const {
	std::vector<std::string> names;
	for (const auto &entry : autoloads) {
		names.push_back(entry.first);
	}
	return names;
}

std::optional<std::string> FakeProject::script_source(const std::string &p_path) const {
	const auto found = sources.find(p_path);
	return found == sources.end() ? std::nullopt : std::optional<std::string>(found->second);
}

// As the editor answers: without loading the script, only what the global
// class list says.
std::optional<compiler::ForeignClass> FakeProject::foreign_class(const std::string &p_path, bool p_members) const {
	const auto found = foreign.find(p_path);
	if (found == foreign.end()) {
		return std::nullopt;
	}
	compiler::ForeignClass result = found->second;
	result.members_known = p_members;
	const auto named = std::find_if(global_classes.begin(), global_classes.end(), [&](const auto &p_entry) { return p_entry.second == p_path; });
	if (named != global_classes.end()) {
		result.name = named->first;
	}
	if (!p_members) {
		result.properties.clear();
		result.methods.clear();
		result.signals.clear();
		result.constants.clear();
		if (named == global_classes.end()) {
			result.native.clear();
		}
	}
	return result;
}

compiler::CompileResult compile_in(const std::string &p_source, const FakeProject &p_project, bool p_declarations_only) {
	compiler::CompileOptions options;
	options.path = "res://test.pet";
	options.project = &p_project;
	options.declarations_only = p_declarations_only;
	check_syntax_conventions(p_source);
	check_formatting(p_source);
	compiler::CompileResult result = compiler::compile(p_source, fake_engine(), options);
	if (result.errors.empty()) {
		check_references(p_source, fake_engine(), options);
		check_recovery(p_source, [&](const std::string &p_cut) { compiler::compile(p_cut, fake_engine(), options); });
	}
	return result;
}

std::vector<compiler::Diagnostic> check_scene(const FakeProject &p_project, const std::string &p_scene) {
	compiler::Analysis analysis(fake_engine(), p_project);
	return analysis.check_scene(p_scene)->errors;
}

std::string messages(const compiler::CompileResult &p_result) {
	std::string text;
	for (const compiler::Diagnostic &d : p_result.errors) {
		text += std::to_string(d.line) + ":" + std::to_string(d.column) + ": " + d.message + "\n";
	}
	return text;
}

std::vector<std::string> errors_of(const std::string &p_source) {
	std::vector<std::string> result;
	for (const compiler::Diagnostic &d : compile(p_source).errors) {
		result.push_back(std::to_string(d.line) + ":" + std::to_string(d.column) + ": " + d.message);
	}
	return result;
}

std::string joined(const std::vector<std::string> &p_lines) {
	std::string result;
	for (const std::string &line : p_lines) {
		result += line + "\n";
	}
	return result;
}

bool contains(const std::string &p_text, const std::string &p_part) {
	return p_text.find(p_part) != std::string::npos;
}

bool has_error(const std::string &p_source, const std::string &p_text) {
	for (const std::string &error : errors_of(p_source)) {
		if (contains(error, p_text)) {
			return true;
		}
	}
	return false;
}

std::string indented(const std::string &p_text) {
	std::string result;
	size_t start = 0;
	while (start <= p_text.size()) {
		const size_t end = p_text.find('\n', start);
		result += "\t" + p_text.substr(start, end == std::string::npos ? std::string::npos : end - start) + "\n";
		if (end == std::string::npos) {
			break;
		}
		start = end + 1;
	}
	return result;
}

std::string in_function(const std::string &p_body, const std::string &p_signature, const std::string &p_base) {
	return "extends " + p_base + "\n\n" + p_signature + "\n" + indented(p_body);
}

int function_index(const bytecode::Module &p_module, const std::string &p_name) {
	for (size_t i = 0; i < p_module.functions.size(); i++) {
		if (p_module.functions[i].name == p_name) {
			return int(i);
		}
	}
	return -1;
}

size_t find_instruction(const bytecode::Function &p_function, bytecode::Op p_op) {
	size_t pc = 0;
	while (pc < p_function.code.size()) {
		if (bytecode::Op(p_function.code[pc]) == p_op) {
			return pc;
		}
		pc += bytecode::instruction_length(p_function.code.data() + pc);
	}
	FAIL("no such instruction");
	return 0;
}

bytecode::ValueType storage(const bytecode::Module &p_module, uint32_t p_type) {
	return bytecode::storage_of(p_module.types, p_type);
}

uint32_t value_type_index(bytecode::Module &r_module, bytecode::ValueType p_type) {
	bytecode::TypeDesc type;
	type.value = p_type;
	for (uint32_t i = 0; i < r_module.types.size(); i++) {
		if (r_module.types[i] == type) {
			return i;
		}
	}
	r_module.types.push_back(type);
	return uint32_t(r_module.types.size() - 1);
}

std::string type_of(const std::string &p_expr, const std::string &p_setup) {
	const std::string body = (p_setup.empty() ? "" : p_setup + "\n") + "var probe: bool = " + p_expr;
	const std::vector<std::string> errors = errors_of(in_function(body, "func f() -> void:", "Node3D"));
	if (errors.empty()) {
		return "bool";
	}
	const std::string prefix = "Cannot assign a value of type ";
	for (const std::string &error : errors) {
		const size_t start = error.find(prefix);
		const size_t end = error.find(" as bool");
		if (start != std::string::npos && end != std::string::npos) {
			return error.substr(start + prefix.size(), end - start - prefix.size());
		}
	}
	return "error: " + errors[0];
}

} // namespace petrichor::tests
