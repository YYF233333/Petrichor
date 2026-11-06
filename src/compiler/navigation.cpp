#include "compiler/navigation.h"

#include "compiler/ast.h"
#include "compiler/builtin_api.h"

#include <algorithm>
#include <cctype>

namespace petrichor::compiler {

namespace {

int line_of(const std::string &p_source, uint32_t p_offset) {
	return 1 + int(std::count(p_source.begin(), p_source.begin() + std::min<size_t>(p_offset, p_source.size()), '\n'));
}

const Reference *declaration_of(const std::vector<Reference> &p_references, const DeclId &p_target) {
	for (const Reference &reference : p_references) {
		if (reference.is_declaration && reference.target == p_target) {
			return &reference;
		}
	}
	return nullptr;
}

// Where the documentation files a declaration of the engine: under its class,
// or under @GlobalScope for the global enums, as "Key" and "Variant.Type".
std::optional<Lookup> engine_lookup(const EngineApi &p_engine, const DeclId &p_target) {
	using Kind = Lookup::Kind;
	const std::string &owner = p_target.scope;
	const std::string &name = p_target.name;
	Lookup found;
	found.class_name = owner;
	found.member = name;
	const builtin::Class *type = builtin::find_class(owner);
	if (builtin::find_enum(name.empty() ? owner : owner + "." + name)) {
		found.kind = Kind::Enum;
		if (name.empty() || !type) {
			found.class_name = "@GlobalScope";
			found.member = name.empty() ? owner : owner + "." + name;
		}
	} else if (name.empty()) {
		found.kind = Kind::Class;
	} else if (owner == "@GlobalScope") {
		found.kind = builtin::find_utility(name) ? Kind::Method : Kind::Constant;
	} else if (type && builtin::find_member(*type, name)) {
		found.kind = Kind::Property;
	} else if (type && builtin::find_method(*type, name)) {
		found.kind = Kind::Method;
	} else if (type) {
		// An enum's values are constants of the type to the documentation.
		found.kind = Kind::Constant;
	} else if (p_engine.find_signal(owner, name)) {
		found.kind = Kind::Signal;
	} else if (p_engine.find_property(owner, name)) {
		found.kind = Kind::Property;
	} else if (p_engine.find_method(owner, name)) {
		found.kind = Kind::Method;
	} else if (p_engine.find_constant(owner, name)) {
		found.kind = Kind::Constant;
	} else if (p_engine.find_enum(owner, name)) {
		found.kind = Kind::Enum;
	} else {
		return std::nullopt;
	}
	return found;
}

bool is_name_char(char p_char) {
	return std::isalnum(static_cast<unsigned char>(p_char)) || p_char == '_';
}

// A member a GDScript file declares outside its inner classes, by its text:
// a line that is not indented and, after annotations and `static`, declares
// the name, as `@export var speed := 2.0` does.
std::optional<Lookup> gdscript_declaration(const std::string &p_source, const std::string &p_name) {
	using Kind = Lookup::Kind;
	static const std::pair<const char *, Kind> KEYWORDS[] = {
		{ "func", Kind::Method },
		{ "var", Kind::Property },
		{ "const", Kind::Constant },
		{ "signal", Kind::Signal },
		{ "enum", Kind::Enum },
		{ "class", Kind::Class },
	};
	const auto word = [&](size_t &r_at) {
		const size_t first = r_at;
		while (r_at < p_source.size() && is_name_char(p_source[r_at])) {
			r_at++;
		}
		return p_source.substr(first, r_at - first);
	};
	const auto blanks = [&](size_t &r_at) {
		while (r_at < p_source.size() && (p_source[r_at] == ' ' || p_source[r_at] == '\t')) {
			r_at++;
		}
	};
	int line = 1;
	for (size_t start = 0; start < p_source.size(); line++) {
		const size_t end = std::min(p_source.find('\n', start), p_source.size());
		size_t at = start;
		while (at < end && p_source[at] == '@') {
			at++;
			word(at);
			for (int depth = 0; at < end && (depth > 0 || p_source[at] == '('); at++) {
				if (p_source[at] == '(') {
					depth++;
				} else if (p_source[at] == ')') {
					depth--;
				}
			}
			blanks(at);
		}
		std::string keyword = word(at);
		if (keyword == "static") {
			blanks(at);
			keyword = word(at);
		}
		blanks(at);
		for (const auto &[spelling, kind] : KEYWORDS) {
			if (keyword == spelling && word(at) == p_name) {
				Lookup found;
				found.kind = kind;
				found.line = line;
				return found;
			}
		}
		start = end + 1;
	}
	return std::nullopt;
}

} // namespace

std::optional<Lookup> lookup(Analysis &r_analysis, const EngineApi &p_engine, const ProjectApi &p_project, const std::string &p_path, const std::string &p_source, uint32_t p_offset) {
	const std::shared_ptr<const CompileResult> result = r_analysis.compile(p_path, p_source);
	const auto at = std::find_if(result->references.begin(), result->references.end(), [&](const Reference &p_reference) {
		return p_reference.scene.empty() && p_reference.begin <= p_offset && p_offset <= p_reference.end;
	});
	if (at == result->references.end()) {
		return std::nullopt;
	}
	DeclId target = at->target;
	if (target.kind == DeclId::Kind::Autoload) {
		// Its script's class; the editor opens a scene itself
		// (editor/script/script_text_editor.cpp:1292-1297).
		const std::string path = p_project.autoload_path(target.name);
		const bool own = path.size() > 4 && path.compare(path.size() - 4, 4, ".pet") == 0;
		if (!own && !p_project.foreign_class(path, false)) {
			return std::nullopt;
		}
		target = { own ? DeclId::Kind::Script : DeclId::Kind::Foreign, path, std::string() };
	}
	switch (target.kind) {
		case DeclId::Kind::Local: {
			if (target.name.empty()) {
				return std::nullopt;
			}
			const Reference *declaration = declaration_of(result->references, target);
			Lookup found;
			found.kind = declaration && declaration->is_constant ? Lookup::Kind::LocalConstant : Lookup::Kind::Local;
			found.type = declaration ? declaration->type : std::string();
			found.path = p_path;
			found.line = line_of(p_source, uint32_t(std::stoul(target.name)));
			return found;
		}
		case DeclId::Kind::Script: {
			const std::string file = script_file(target.scope);
			const std::optional<std::string> source = file == p_path ? p_source : p_project.script_source(file);
			if (!source) {
				return std::nullopt;
			}
			// Each file records its own declarations.
			const std::shared_ptr<const CompileResult> declared_in = file == p_path ? result : r_analysis.compile(file, *source);
			const Reference *declaration = declaration_of(declared_in->references, target);
			if (!declaration && !target.name.empty()) {
				return std::nullopt;
			}
			Lookup found;
			found.path = file;
			found.line = declaration ? line_of(*source, declaration->begin) : 1;
			return found;
		}
		case DeclId::Kind::Foreign: {
			// The script that declares it, along the scripts the class extends.
			for (std::string script = target.scope; !script.empty();) {
				const std::optional<std::string> source = p_project.script_source(script);
				// With members, which says the script's base also without a class_name.
				const std::optional<ForeignClass> foreign = p_project.foreign_class(script, true);
				if (!source || !foreign) {
					return std::nullopt;
				}
				std::optional<Lookup> found;
				if (target.name.empty()) {
					found.emplace();
					found->kind = Lookup::Kind::Class;
					found->line = 1;
				} else {
					found = gdscript_declaration(*source, target.name);
				}
				if (found) {
					found->member = target.name;
					found->path = script;
					return found;
				}
				script = foreign->base;
			}
			return std::nullopt;
		}
		case DeclId::Kind::Engine:
			return engine_lookup(p_engine, target);
		default:
			return std::nullopt;
	}
}

} // namespace petrichor::compiler
