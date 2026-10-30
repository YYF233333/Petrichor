#include "bytecode/opcodes.h"
#include "doctest.h"
#include "helpers.h"

using namespace petrichor;
using namespace petrichor::tests;

namespace {
FakeProject inheritance_project() {
	FakeProject p;
	p.add_script("res://base.pet", R"(class_name Base
extends Node
pub var count: int = 4
var hidden: int = 7
pub signal tick(value: int)
pub func choose(value: Node3D, extra: int = 1) -> Node:
	return value
func secret() -> int:
	return hidden
pub func local() -> int:
	return secret()
func _process(delta):
	pass
)",
			"Base");
	p.add_script("res://middle.pet", R"(class_name Middle
extends Base
pub var more: String = "middle"
pub func choose(value: Node, extra = 2, last: int = 3) -> Node3D:
	return (value as Node3D).unwrap()
)",
			"Middle");
	p.add_script("res://sibling.pet", "class_name Sibling\nextends Base\n", "Sibling");
	return p;
}
} //namespace

TEST_CASE("inheritance keeps member prefixes and inherited signatures") {
	auto p = inheritance_project();
	const auto r = compile_in(R"(class_name Leaf
extends "middle.pet"
var own: int = hidden
pub func choose(value, extra = 4, last = 5):
	return super.choose(value, extra, last)
func _init():
	super()
pub func read() -> int:
	return secret() + count + own
)",
			p);
	INFO(messages(r));
	REQUIRE(r.ok());
	CHECK(r.module.base_script == "res://middle.pet");
	CHECK(r.module.base_class == "Node");
	CHECK(r.module.inherited_members == 3);
	REQUIRE(r.module.members.size() == 4);
	CHECK(r.module.members[1].name == "hidden");
	CHECK(r.module.members[3].offset > r.module.members[2].offset);
	bool inherited = false, override = false;
	for (const auto &fn : r.module.functions) {
		if (fn.name == "secret") {
			inherited = fn.is_inherited && fn.code.empty() && !fn.is_override;
		}
		if (fn.name == "choose") {
			override = !fn.is_inherited && fn.params.size() == 3 && fn.params[0].class_name == "Node" && fn.result.class_name == "Node3D";
		}
	}
	CHECK(inherited);
	CHECK(override);
	std::string error;
	CHECK(bytecode::verify(r.module, error));
	auto bytes = bytecode::serialize(r.module);
	bytecode::Module decoded;
	CHECK(bytecode::deserialize(bytes.data(), bytes.size(), decoded, error));
	CHECK(decoded.base_script == r.module.base_script);
}

TEST_CASE("inheritance rejects incompatible overrides and member collisions") {
	auto p = inheritance_project();
	const auto errors = [&](const std::string &body) { return messages(compile_in("extends Base\n" + body, p)); };
	CHECK(contains(errors("func choose(value: Node3D, extra: int = 1) -> Node:\n\treturn value\n"), "visibility"));
	CHECK(contains(errors("pub func secret() -> int:\n\treturn 1\n"), "visibility"));
	CHECK(contains(errors("pub func choose(value: Node3D, extra: int) -> Node:\n\treturn value\n"), "argument count"));
	CHECK(contains(errors("var hidden: int = 2\n"), "already declared"));
	CHECK(contains(errors("signal count()\n"), "already declared"));
	CHECK(contains(errors("func tick():\n\tpass\n"), "already declared"));
	CHECK(contains(errors("pub async func choose(value: Node3D, extra: int = 1) -> Node:\n\treturn value\n"), "static and async"));
	p.add_script("res://numeric.pet", "class_name Numeric\nfunc n(value: int) -> float:\n\treturn 1.0\n", "Numeric");
	CHECK(contains(messages(compile_in("extends Numeric\nfunc n(value: float) -> int:\n\treturn 1\n", p)), "narrows"));
}

TEST_CASE("inheritance subtyping and common ancestors retain script identity") {
	auto p = inheritance_project();
	const auto r = compile_in(R"(extends Node
func common(a: Middle, b: Sibling, yes: bool) -> Base:
	var joined = a if yes else b
	var n: Node = joined
	return joined
func call(a: Middle) -> int:
	return a.local() + a.count
)",
			p);
	INFO(messages(r));
	CHECK(r.ok());
	CHECK(contains(messages(compile_in("func no(a: Middle) -> int:\n\treturn a.secret()\n", p)), "not pub"));
	p.add_script("res://contract.pet", "class_name Contract\npub interface Hit:\n\tfunc hit(value: int)\n", "Contract");
	p.add_script("res://impl.pet", "class_name Impl\nextends Node implements Contract.Hit\npub func hit(value: int):\n\tpass\n", "Impl");
	p.add_script("res://child.pet", "class_name Child\nextends Impl\n", "Child");
	CHECK(compile_in("func yes(value: Child) -> Contract.Hit:\n\treturn value\n", p).ok());
}

TEST_CASE("inheritance rejects cycles foreign bases and missing super implementations") {
	auto p = inheritance_project();
	p.add_script("res://a.pet", "class_name A\nextends B\n", "A");
	p.add_script("res://b.pet", "class_name B\nextends A\n", "B");
	CHECK(contains(messages(compile_in("extends A\n", p)), "Inheritance cycle"));
	p.global_classes["Foreign"] = "res://foreign.gd";
	CHECK(contains(messages(compile_in("extends Foreign\n", p)), "another language"));
	CHECK(contains(messages(compile_in("extends Node\nfunc _ready():\n\tsuper()\n", p)), "No base implementation"));
	CHECK(compile_in("extends Node\nfunc _init():\n\tsuper()\n", p).ok());
	CHECK(contains(messages(compile_in("extends Base\nstatic func bad():\n\tsuper.secret()\n", p)), "belongs to an instance"));
}

TEST_CASE("inheritance scene connections belong to their attached class") {
	FakeProject p;
	p.add_script("res://base.pet", "class_name Base\nextends Node\nsignal tick\nfunc base_connected():\n\tpass\n", "Base");
	p.add_script("res://child.pet", "extends Base\nfunc child_connected():\n\tpass\n");
	compiler::Scene base;
	base.path = "res://base.tscn";
	base.nodes = { { ".", "Node", "res://base.pet" } };
	base.connections = { { "tick", ".", ".", "base_connected" } };
	compiler::Scene child;
	child.path = "res://child.tscn";
	child.nodes = { { ".", "Node", "res://child.pet" } };
	child.connections = { { "tick", ".", ".", "child_connected" } };
	p.scenes = { base, child };
	for (const std::string path : { "res://base.pet", "res://child.pet" }) {
		compiler::CompileOptions options;
		options.path = path;
		options.project = &p;
		const auto r = compiler::compile(p.sources.at(path), fake_engine(), options);
		INFO(messages(r));
		REQUIRE(r.ok());
		CHECK(function_named(r.module, "base_connected").is_exposed);
		if (path == "res://child.pet") {
			CHECK(function_named(r.module, "child_connected").is_exposed);
		}
	}
}

TEST_CASE("inheritance super object arguments stay inside the language") {
	FakeProject p;
	p.add_script("res://base.pet", "class_name Base\nextends Node\nfunc forward(value: Node) -> Node:\n\treturn value\n", "Base");
	const auto r = compile_in("extends Base\nfunc forward(value: Node) -> Node:\n\treturn super(value)\n", p);
	INFO(messages(r));
	REQUIRE(r.ok());
	const auto &code = function_named(r.module, "forward").code;
	bool has_super = false;
	for (size_t pc = 0; pc < code.size(); pc += bytecode::instruction_length(code.data() + pc)) {
		CHECK(bytecode::Op(code[pc]) != bytecode::Op::CHECK_LIVE);
		has_super |= bytecode::Op(code[pc]) == bytecode::Op::CALL_SUPER;
	}
	CHECK(has_super);
}
