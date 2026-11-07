#include "doctest.h"
#include "helpers.h"

using namespace petrichor;
using namespace petrichor::tests;

// Structs (spec §2.4, §3.6, §4.3).

namespace {

const char *const HIT = "struct Hit:\n\tvar damage: int\n\tvar point: Vector3\n\tvar crit: bool = false\n\nstruct Settings:\n\tvar volume: float = 0.5\n\tvar name: String = \"default\"\n\n";

std::string errors(const std::string &p_body, const std::string &p_members = "") {
	return joined(errors_of(std::string("class_name Shots\nextends Node\n\n") + HIT + p_members + "func f(a: Hit, b: Hit) -> void:\n" + indented(p_body)));
}

FakeProject project() {
	FakeProject project;
	project.add_script("res://combat.pet", "class_name Combat\n\npub struct Hit:\n\tvar damage: int\n\tvar crit: bool = false\n\nstruct Secret:\n\tvar code: int = 0\n", "Combat");
	return project;
}

} // namespace

TEST_CASE("a struct is constructed with its fields by name, and those with defaults may be left out") {
	CHECK(errors("var h := Hit(damage = 3, point = Vector3.ZERO)\nvar s := Settings(name = \"loud\")\nvar t := Settings()\nh.damage += s.name.length()").empty());
	CHECK(contains(errors("var h := Hit(3, Vector3.ZERO)"), "A struct takes its fields by name, as in Shots.Hit(damage = ...)"));
	CHECK(contains(errors("var h := Hit(damage = 3)"), "needs \"point\", which has no default value"));
	CHECK(contains(errors("var h := Hit(damage = 3, point = Vector3.ZERO, speed = 2)"), "has no field \"speed\""));
	CHECK(contains(errors("var h := Hit(damage = \"3\", point = Vector3.ZERO)"), "Cannot pass a value of type String as int"));
	CHECK(contains(errors("print(a.speed)"), "has no field \"speed\""));
	CHECK(contains(errors("add_child(Node.new(), force_readable_name = true)"), "takes them by position"));
}

TEST_CASE("a field is changed where its struct lives, never in a temporary") {
	const std::string make = "func make() -> Hit:\n\treturn Hit(damage = 1, point = Vector3.ZERO)\n\n";
	CHECK(errors("a.damage = 2\na.point.x = 1.0\na.point.x += 1.0", make).empty());
	CHECK(contains(errors("make().damage = 3", make), "temporary"));
}

TEST_CASE("structs compare field by field") {
	CHECK(errors("var same: bool = a == b\nvar other: bool = a != b").empty());
	CHECK(contains(errors("var s := Settings()\nvar x: bool = a == s"), "Invalid operands"));
}

TEST_CASE("a struct starts at its default only when every field has one") {
	CHECK(errors("var s: Settings\nprint(s.volume)", "var settings: Settings\n\n").empty());
	CHECK(contains(errors("var h: Hit"), "\"h\" of type Shots.Hit needs an initial value"));
	CHECK(errors("pass", "var last: Hit\n\n").empty()); // One that must be assigned (spec §6.2).
	CHECK(contains(errors("pass", "@export var settings: Settings\n\n"), "Structs cannot be exported"));
	CHECK(contains(errors("pass", "struct Odd:\n\tvar n: int = randi()\n\n"), "Non-constant default values are not supported yet."));
}

TEST_CASE("a struct that contains itself without a container has no size") {
	CHECK(contains(errors("pass", "struct Chain:\n\tvar next: Chain?\n\n"), "Chain contains itself through Chain.next"));
	CHECK(contains(errors("pass", "struct Outer:\n\tvar inner: Inner\n\nstruct Inner:\n\tvar outer: Outer\n\n"), "Outer contains itself through Outer.inner, Inner.outer"));
}

TEST_CASE("another class's struct is written ClassName.Name, and must be pub") {
	const compiler::CompileResult ok = compile_in("extends Node\n\nfunc f() -> Combat.Hit:\n\tvar h := Combat.Hit(damage = 2)\n\th.crit = true\n\treturn h\n", project());
	CHECK_MESSAGE(ok.ok(), messages(ok));
	CHECK(contains(messages(compile_in("extends Node\n\nfunc f() -> void:\n\tvar s := Combat.Secret()\n", project())), "Combat.Secret is not pub"));
}

// Spec §3.8: containers keep structs boxed.
TEST_CASE("containers of structs") {
	CHECK(errors("var hits: Array[Hit] = [a, b]\nhits.append(a)\nhits[0].damage = 3\nvar found: bool = hits.has(b)\nvar first: Hit = hits[0]\nfor h in hits:\n\tprint(h.damage)\n"
				 "var by_name: Dictionary[String, Hit] = {\"a\": a}\nby_name[\"b\"] = b\nvar keyed: Dictionary[Hit, int] = {a: 1}\nkeyed[b] = 2\nvar n: int = keyed[a]\n"
				 "var nested: Array[Array[Hit]] = [hits]\nvar s: Array[Settings] = []\ns.resize(2)\nvar u: Array = Array(hits)\nvar e: Dictionary = Dictionary(by_name)")
					.empty());
	CHECK(contains(errors("var hits: Array[Hit] = []\nhits.resize(2)"), "give every field of Shots.Hit a default value"));
	CHECK(contains(errors("var hits: Array[Hit] = []\nprint(hits)"), "convert it explicitly with Array(x)"));
	// A copy keeps its boxes, so it keeps its type too.
	CHECK(errors("var hits: Array[Hit] = []\nvar c: Array[Hit] = hits.duplicate_deep()").empty());
	CHECK(contains(errors("var hits: Array[Hit] = []\nvar u: Array = hits.duplicate_deep()"), "as Array. Its elements exist only in this language; convert it explicitly with Array(x)"));
	CHECK(contains(errors("var keyed: Dictionary[Hit, int] = {}\nvar u: Dictionary = keyed"), "convert it explicitly with Dictionary(x)"));
	CHECK(contains(errors("pass", "@export var hits: Array[Settings] = []\n\n"), "cannot be exported"));
	const compiler::CompileResult result = compile(std::string("extends Node\n\n") + HIT + "func f(a: Hit) -> int:\n\tvar hits: Array[Hit] = [a]\n\tvar keyed: Dictionary[Hit, int] = {a: 1}\n\tvar s: Array[Settings] = []\n\ts.resize(2)\n\tvar u: Array = Array(hits)\n\treturn keyed[hits[0]]\n");
	REQUIRE_MESSAGE(result.ok(), messages(result));
	std::string error;
	CHECK_MESSAGE(bytecode::verify(result.module, error), error);
	find_instruction(function_named(result.module, "f"), bytecode::Op::ENCODE);
}

// Spec §3.6, §5.2: by the fields in order, when all of them are ordered.
TEST_CASE("structs and T? are ordered") {
	const std::string tagged = "struct Tagged:\n\tvar node: Node\n\n";
	CHECK(errors("var x: bool = a < b and a >= b and a > b and a <= b\nvar hits: Array[Hit] = [a, b]\nhits.sort()\nvar i := hits.bsearch(a)\nvar m: Hit = hits.min()\nvar later: bool = hits < hits").empty());
	CHECK(errors("var p: int? = 1\nvar q: int? = None\nvar x: bool = p < q and p == q and p == 3 and q != 2").empty());
	CHECK(contains(errors("var p: int? = 1\nvar x: bool = p < 3"), "The value may be None"));
	CHECK(contains(errors("var t := Tagged(node = self)\nvar x: bool = t < t", tagged), "Values of type Shots.Tagged are not ordered"));
	CHECK(contains(errors("var ts: Array[Tagged] = []\nts.sort()", tagged), "orders the elements by <, which Shots.Tagged does not have"));
	CHECK(contains(errors("var ns: Array[Node] = []\nvar x: bool = ns < ns"), "Values of type Array[Node] are not ordered"));
	// One that holds itself through a container is ordered when the rest is (spec §3.6).
	CHECK_FALSE(contains(errors("var ts: Array[Tree] = []\nts.sort()", "struct Tree:\n\tvar size: int = 0\n\tvar children: Array[Tree] = []\n\n"), "orders the elements"));
}

// Spec §3.6: methods read the fields bare or through self, and change none.
const char *const SCORE = "struct Score:\n\tvar points: int\n\tvar bonus: bool = false\n\tvar marks: PackedInt32Array = PackedInt32Array()\n\tvar log: Array[int] = []\n\n"
						  "\tfunc total(times: int = 1) -> int:\n\t\treturn (points * 2 if bonus else points) * times\n\n"
						  "\tfunc doubled() -> Score:\n\t\treturn Score(points = self.points * 2, bonus = bonus)\n\n"
						  "\tfunc later() -> func() -> int:\n\t\treturn func(): return total() + points\n\n"
						  "\tstatic func zero() -> Score:\n\t\treturn Score(points = 0)\n\n";

TEST_CASE("a struct's methods read its fields bare or through self, and are called on its values") {
	const compiler::CompileResult result = compile(std::string("extends Node\n\n") + SCORE + "func f(s: Score) -> int:\n\treturn s.total() + s.doubled().total(3) + Score.zero().total() + s.zero().points + s.later().call()\n");
	REQUIRE_MESSAGE(result.ok(), messages(result));
	std::string error;
	CHECK_MESSAGE(bytecode::verify(result.module, error), error);
	const bytecode::Function &total = function_named(result.module, "Score.total");
	CHECK(total.is_static);
	CHECK(total.params.size() == 2);
	CHECK(function_named(result.module, "Score.zero").params.empty());
	find_instruction(function_named(result.module, "f"), bytecode::Op::CALL);
}

TEST_CASE("a struct's methods are values, an instance method with a copy of the struct (spec §3.11)") {
	const compiler::CompileResult result = compile_in(std::string("extends Node\n\n") + SCORE + "func f(s: Score) -> int:\n\tvar total := s.total\n\tvar zero := Score.zero\n\tvar inside := s.later\n\treturn total.call(2) + zero.call().points\n", FakeProject());
	REQUIRE_MESSAGE(result.ok(), messages(result));
	std::string error;
	CHECK_MESSAGE(bytecode::verify(result.module, error), error);
	const bytecode::Function &f = function_named(result.module, "f");
	CHECK(result.module.method_calls[f.code[find_instruction(f, bytecode::Op::STRUCT_METHOD_VALUE) + 1]].method == "Score.total");
	find_instruction(f, bytecode::Op::STATIC_VALUE);
	// Bare in a method, on self.
	CHECK(errors("pass", std::string(SCORE) + "\tfunc each() -> func(int) -> int:\n\t\treturn total\n\n").empty());
}

// Score with only its fields and `p_head`, a method with the lines of `p_body`.
static std::string score_with(const std::string &p_head, const std::string &p_body) {
	const std::string score = SCORE;
	std::string method = "\t" + p_head + ":\n";
	for (size_t start = 0; start <= p_body.size();) {
		const size_t end = std::min(p_body.find('\n', start), p_body.size());
		method += "\t\t" + p_body.substr(start, end - start) + "\n";
		start = end + 1;
	}
	return score.substr(0, score.find("\n\n") + 2) + method + "\n";
}

TEST_CASE("a struct's methods do not change self") {
	for (const std::string body : { "points = 3", "self.points += 1", "marks.append(1)", "self.marks[0] = 2", "var f := func(): points = 1" }) {
		const std::string found = errors("pass", score_with("func change() -> void", body));
		CHECK_MESSAGE(contains(found, "A struct's methods do not change self (spec §3.6)."), body, ": ", found);
	}
	CHECK(errors("pass", score_with("func change() -> void", "log.append(1)\nvar copy := self\ncopy.points = 2")).empty());
}

TEST_CASE("a struct's methods reach of the class only what needs no object") {
	const std::string around = "var hp := 3\nconst MAX = 9\n\nstatic func cap() -> int:\n\treturn MAX\n\n";
	const auto method = [&](const std::string &p_body, const std::string &p_head = "func check() -> void") {
		return errors("pass", around + score_with(p_head, p_body));
	};
	CHECK(method("print(MAX + cap())").empty());
	CHECK(contains(method("print(hp)"), "\"hp\" belongs to an object of Shots, which Score does not have"));
	CHECK(contains(method("print(get_child_count())"), "which Score does not have"));
	CHECK(contains(method("print(points)", "static func check() -> void"), "\"points\" is a field of self, which the static function \"Score.check()\" does not have."));
	CHECK(contains(method("print(self)", "static func check() -> void"), "has no self"));
	CHECK(contains(errors("print(Score.total())", SCORE), "\"total()\" of Shots.Score is not static; call it on a value."));
	CHECK(contains(errors("var f := Score.total", SCORE), "\"total()\" of Shots.Score is not static; take it from a value."));
	CHECK(contains(errors("print(Score.zero().missing())", SCORE), "Shots.Score has no method \"missing()\"."));
	CHECK(contains(errors("pass", "struct Open:\n\tpub func f() -> void:\n\t\tpass\n\n"), "they take no pub"));
}

TEST_CASE("another class's struct has its methods called on that class's image") {
	FakeProject project;
	project.add_script("res://tally.pet", std::string("class_name Tally\n\npub ") + SCORE, "Tally");
	const compiler::CompileResult result = compile_in("extends Node\n\nfunc f() -> int:\n\tvar s := Tally.Score.zero()\n\treturn s.total(2)\n", project);
	REQUIRE_MESSAGE(result.ok(), messages(result));
	std::string error;
	CHECK_MESSAGE(bytecode::verify(result.module, error), error);
	find_instruction(function_named(result.module, "f"), bytecode::Op::CALL_METHOD);
}

TEST_CASE("a field of a struct in a register is a slot inside it, which the verifier accepts") {
	const compiler::CompileResult result = compile(std::string("extends Node\n\n") + HIT + "func f(a: Hit) -> bool:\n\tvar h := a\n\th.crit = h.damage > 2\n\th.point.y = 1.0\n\treturn h == a\n");
	REQUIRE_MESSAGE(result.ok(), messages(result));
	std::string error;
	CHECK_MESSAGE(bytecode::verify(result.module, error), error);
	const bytecode::Function &f = function_named(result.module, "f");
	find_instruction(f, bytecode::Op::EQUAL);
}
