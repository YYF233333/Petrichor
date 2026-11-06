#include "compiler/navigation.h"

#include "compiler/ast.h"
#include "compiler/builtin_api.h"
#include "compiler/lexer.h"

#include <algorithm>
#include <cctype>

namespace petrichor::compiler {

namespace {

using Kind = SymbolKind;

int line_of(const std::string &p_source, uint32_t p_offset) {
	return 1 + int(std::count(p_source.begin(), p_source.begin() + std::min<size_t>(p_offset, p_source.size()), '\n'));
}

const Reference *declaration_of(const std::vector<Reference> &p_references, const DeclId &p_target) {
	for (const Reference &reference : p_references) {
		if (reference.declares && reference.target == p_target) {
			return &reference;
		}
	}
	return nullptr;
}

// Where the documentation files a declaration of the engine: under its class,
// or under @GlobalScope for the global enums, as "Key" and "Variant.Type".
std::optional<Lookup> engine_lookup(const EngineApi &p_engine, const DeclId &p_target) {
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

std::string doc_class_name(const DocName &p_doc_name, const std::string &p_path) {
	std::string name = p_doc_name(script_file(p_path));
	for (size_t at = p_path.find("::"); at != std::string::npos;) {
		const size_t next = p_path.find("::", at + 2);
		name += "." + p_path.substr(at + 2, next == std::string::npos ? next : next - at - 2);
		at = next;
	}
	return name;
}

std::optional<Lookup> lookup(Analysis &r_analysis, const EngineApi &p_engine, const ProjectApi &p_project, const DocName &p_doc_name, const std::string &p_path, const std::string &p_source, uint32_t p_offset) {
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
			found.kind = declaration ? *declaration->declares : Kind::Local;
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
			found.kind = declaration ? *declaration->declares : Kind::Class;
			found.class_name = doc_class_name(p_doc_name, target.scope);
			found.member = target.name;
			// A field of a struct or a member of an interface is in that one's
			// documentation, a value of an enum in the class's.
			const size_t dot = found.member.find('.');
			if (dot != std::string::npos) {
				if (found.kind != Kind::Constant) {
					found.class_name += "." + found.member.substr(0, dot);
				}
				found.member = found.member.substr(dot + 1);
			}
			if (found.kind == Kind::Class && !found.member.empty()) {
				found.class_name += "." + found.member;
				found.member.clear();
			}
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
					found->kind = Kind::Class;
					found->line = 1;
				} else {
					found = gdscript_declaration(*source, target.name);
				}
				if (found) {
					found->class_name = p_doc_name(script);
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

namespace {

struct Documenter {
	const SourceFile &source;
	const DocName &doc_name;
	std::vector<ClassDoc> docs;

	std::string text(const std::unique_ptr<Expr> &p_expr) const {
		if (!p_expr) {
			return std::string();
		}
		const uint32_t begin = source.begin(p_expr->range);
		return source.text.substr(begin, source.end(p_expr->range) - begin);
	}

	// As this language writes the type, but a typed array as `T[]` and an enum
	// as int with the enum's name, which the documentation links
	// (editor/doc/editor_help.cpp:410-470).
	DocMember typed(const std::string &p_name, const Type &p_type) const {
		DocMember member;
		member.name = p_name;
		if (p_type.kind == TypeKind::Enum) {
			member.type = "int";
			member.enumeration = p_type.script.empty() ? p_type.name : doc_class_name(doc_name, p_type.script) + "." + p_type.interface_name();
		} else if (p_type.kind == TypeKind::Builtin && p_type.name == "Array" && p_type.args.size() == 1) {
			member.type = p_type.args[0].to_string() + "[]";
		} else {
			member.type = p_type.to_string();
		}
		return member;
	}

	DocMember function(const FuncDecl &p_function) const {
		DocMember method = typed(p_function.name, p_function.return_type);
		for (const char *qualifier : { p_function.is_static ? "static" : "", p_function.is_async ? "async" : "", p_function.is_abstract ? "abstract" : "" }) {
			if (*qualifier) {
				method.qualifiers += (method.qualifiers.empty() ? "" : " ") + std::string(qualifier);
			}
		}
		for (const ParamDecl &param : p_function.params) {
			method.arguments.push_back(typed(param.name, param.type));
			method.arguments.back().value = text(param.default_value);
		}
		return method;
	}

	void document(const ClassDecl &p_class, const std::string &p_path) {
		for (const std::unique_ptr<ClassDecl> &inner : p_class.classes) {
			document(*inner, p_path + "::" + inner->name);
		}
		const std::string name = doc_class_name(doc_name, p_path);
		for (const StructDecl &decl : p_class.structs) {
			ClassDoc doc;
			doc.name = name + "." + decl.name;
			for (const VarDecl &field : decl.fields) {
				doc.properties.push_back(typed(field.name, field.type));
				doc.properties.back().value = text(field.initializer);
			}
			docs.push_back(std::move(doc));
		}
		for (const InterfaceDecl &decl : p_class.interfaces) {
			ClassDoc doc;
			doc.name = name + "." + decl.name;
			for (const FuncDecl &method : decl.methods) {
				doc.methods.push_back(function(method));
			}
			for (const InterfaceProperty &property : decl.properties) {
				doc.properties.push_back(typed(property.name, property.type));
			}
			docs.push_back(std::move(doc));
		}
		ClassDoc doc;
		doc.name = name;
		doc.inherits = p_class.base_script.empty() ? p_class.extends : doc_class_name(doc_name, p_class.base_script);
		for (const ConstDecl &constant : p_class.constants) {
			doc.constants.push_back(typed(constant.name, constant.type));
			doc.constants.back().value = text(constant.initializer);
		}
		for (const EnumDecl &decl : p_class.enums) {
			if (!decl.name.empty()) {
				doc.enums.push_back(decl.name);
			}
			for (const EnumValueDecl &value : decl.values) {
				DocMember constant = typed(value.name, decl.type);
				if (decl.numbers) {
					for (const auto &[number_name, number] : decl.numbers->list) {
						if (number_name == value.name) {
							constant.value = std::to_string(number);
						}
					}
				}
				doc.constants.push_back(std::move(constant));
			}
		}
		for (const std::vector<VarDecl> *variables : { &p_class.members, &p_class.static_vars }) {
			for (const VarDecl &variable : *variables) {
				doc.properties.push_back(typed(variable.name, variable.type));
				doc.properties.back().value = text(variable.initializer);
			}
		}
		for (const FuncDecl &method : p_class.functions) {
			// Not the setters and getters written inside a property, named @set_x
			// and @get_x, nor what a syntax error left of a function.
			if (!method.name.empty() && method.name[0] != '@') {
				doc.methods.push_back(function(method));
			}
		}
		for (const SignalDecl &signal : p_class.signals) {
			DocMember documented;
			documented.name = signal.name;
			for (const ParamDecl &param : signal.params) {
				documented.arguments.push_back(typed(param.name, param.type));
			}
			doc.signals.push_back(std::move(documented));
		}
		docs.push_back(std::move(doc));
	}
};

} // namespace

std::vector<ClassDoc> document(const ClassDecl &p_class, const std::string &p_path, const DocName &p_doc_name) {
	Documenter documenter{ *p_class.source, p_doc_name, {} };
	documenter.document(p_class, p_path);
	return documenter.docs;
}

} // namespace petrichor::compiler
