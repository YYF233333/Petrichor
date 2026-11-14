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
	bool inherited = false, override = false;
	for (const auto &fn : r.module.functions) {
		if (fn.name == "secret") {
			inherited = fn.is_inherited && fn.code.empty() && !fn.is_override;
		}
		if (fn.name == "choose") {
			override = !fn.is_inherited && fn.params.size() == 3 && r.module.types[fn.params[0].type].object.name == "Node" && r.module.types[fn.result.type].object.name == "Node3D";
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

// Spec §4.1, as GDScript checks it (modules/gdscript/gdscript_analyzer.cpp:234-253,
// 291-301; ed1daf0): only a function overrides.
TEST_CASE("a member does not take the name of a member of a base class, except a function") {
	FakeProject p;
	p.add_script("res://states.pet", "class_name States\nextends Node\nenum State { A, B }\nenum { IDLE }\nconst LIMIT := 1\nclass Part:\n\tpass\nfunc act() -> void:\n\tpass\n", "States");
	const auto taken = [&](const std::string &p_member) { return contains(messages(compile_in("extends States\n" + p_member + "\n", p)), "is already declared."); };
	CHECK(taken("func State(x: int) -> int:\n\treturn x"));
	CHECK(taken("var IDLE := 1"));
	CHECK(taken("static var Part := 1"));
	CHECK(taken("const act := 1"));
	CHECK(taken("signal State"));
	CHECK(taken("enum Part { X }"));
	CHECK(taken("class LIMIT:\n\tpass"));
	CHECK(compile_in("extends States\nfunc act() -> void:\n\tpass\n", p).ok());
	CHECK(contains(messages(compile_in("extends Node\nstatic var x := 1\nsignal x\n", p)), "\"x\" is already declared."));
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
	const std::string front = "class_name Front\nclass Z:\n\tpass\nclass X extends Back.Y:\n\tpass\n";
	p.add_script("res://test.pet", front, "Front");
	p.add_script("res://back.pet", "class_name Back\nclass Y extends Side.W:\n\tpass\n", "Back");
	p.add_script("res://side.pet", "class_name Side\nclass W extends Front.Z:\n\tpass\n", "Side");
	CHECK(contains(messages(compile_in(front, p)), "A class of res://back.pet extends one of this file through res://side.pet;"));
	p.global_classes["Foreign"] = "res://foreign.gd";
	CHECK(contains(messages(compile_in("extends Foreign\n", p)), "another language"));
	CHECK(contains(messages(compile_in("extends Node\nfunc _ready():\n\tsuper()\n", p)), "No base implementation"));
	CHECK(compile_in("extends Node\nfunc _init():\n\tsuper()\n", p).ok());
	CHECK(contains(messages(compile_in("extends Base\nstatic func bad():\n\tsuper.secret()\n", p)), "belongs to an instance"));
}

TEST_CASE("inheritance scene connections belong to their attached class") {
	FakeProject p;
	p.add_script("res://base.pet", "class_name Base\nextends Node\npub signal tick\npub func base_connected():\n\tpass\n", "Base");
	p.add_script("res://child.pet", "extends Base\npub func child_connected():\n\tpass\n");
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
		CHECK(r.ok());
	}
}

// Spec §4.1, as GDScript checks it (modules/gdscript/gdscript_analyzer.cpp:1532-1556,
// 2008-2022, 3630-3644; modules/gdscript/gdscript_parser.cpp:715-754, 1773-1774, 4500-4524).
TEST_CASE("an abstract class declares methods its subclasses implement") {
	FakeProject p;
	p.add_script("res://action.pet", "@abstract\nclass_name Action\nextends Resource\n@export pub var label: String\n\n@abstract pub async func execute() -> int\n\n@abstract\nfunc cost() -> int\n\npub func describe() -> String:\n\treturn label + str(cost())\n", "Action");
	const auto action = compile_in(p.sources.at("res://action.pet"), FakeProject());
	REQUIRE_MESSAGE(action.ok(), messages(action));
	CHECK(action.module.is_abstract);
	CHECK((function_named(action.module, "cost").is_abstract && function_named(action.module, "cost").code.empty()));
	// @abstract on its own line was once taken for @rpc.
	CHECK(function_named(action.module, "cost").rpc.mode == 0);
	CHECK(!function_named(action.module, "describe").is_abstract);

	const auto child = compile_in("extends Action\npub async func execute() -> int:\n\treturn 1\n\nfunc cost() -> int:\n\treturn 2\n", p);
	REQUIRE_MESSAGE(child.ok(), messages(child));
	CHECK(!child.module.is_abstract);
	CHECK(compile_in("@abstract\nextends Action\nfunc cost() -> int:\n\treturn 2\n", p).ok());

	CHECK(contains(messages(compile_in("extends Action\nfunc cost() -> int:\n\treturn 2\n", p)), "must implement \"Action.execute()\", and the other abstract methods it inherits, or be marked @abstract."));
	CHECK(contains(messages(compile_in("extends Node\n@abstract func f()\n", p)), "is not abstract but declares the abstract \"f()\"; mark the class @abstract."));
	CHECK(contains(messages(compile_in("@abstract\nextends Node\n@abstract func f():\n\tpass\n", p)), "An abstract function has no body"));
	CHECK(contains(messages(compile_in("@abstract\nextends Node\n@abstract static func f()\n", p)), "A static function cannot be @abstract."));
	CHECK(contains(messages(compile_in("extends Node\n@abstract var x := 1\n", p)), "@abstract applies to classes and functions."));
	CHECK(contains(messages(compile_in("extends Node\nfunc f():\n\tvar a := Action.new()\n", p)), "Action is abstract and cannot be instantiated"));
	CHECK(contains(messages(compile_in("extends Action\npub async func execute() -> int:\n\treturn 1\n\nfunc cost() -> int:\n\treturn super()\n", p)), "\"cost()\" is abstract in the base class"));
	// Before a function it is the function's, even at the top of the file.
	CHECK(contains(messages(compile_in("@abstract func f()\n", p)), "is not abstract but declares the abstract"));
}
