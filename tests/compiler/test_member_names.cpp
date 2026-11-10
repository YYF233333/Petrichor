#include "doctest.h"
#include "helpers.h"

using namespace petrichor;
using namespace petrichor::tests;

// Member names in strings (spec §10.4): a literal name at a position the
// binding metadata lists is a static reference, checked as `object.name`.

namespace {

std::string errors_in(const std::string &p_body) {
	return messages(compile_in("extends Node2D\n\nsignal hit(amount: int)\npub signal done\npub var speed := 1.0\nvar secret := 2\n\npub func reset():\n\tpass\n\nfunc f(other: Node2D, any: Object, tween: Tween):\n\t" + p_body + "\n", FakeProject()));
}

} // namespace

TEST_CASE("a literal member name is checked on the object's static type, as object.name is") {
	CHECK(errors_in("set_deferred(\"speed\", 2.0)").empty());
	CHECK(contains(errors_in("set_deferred(\"sped\", 2.0)"), "has no property \"sped\"."));
	CHECK(errors_in("other.set_deferred(\"position\", Vector2.ZERO)").empty());
	CHECK(contains(errors_in("other.set_deferred(\"speed\", 2.0)"), "Node2D has no property \"speed\"."));
	CHECK(contains(errors_in("call_deferred(\"rest\")"), "has no method \"rest\"."));
	CHECK(contains(errors_in("other.emit_signal(\"done\")"), "Node2D has no signal \"done\"."));
	// The engine finds no member that is not pub (spec §10.5).
	CHECK(contains(errors_in("set_deferred(\"secret\", 3)"), "\"secret\" is not pub, so the engine cannot find it by name; make it pub."));
	CHECK(contains(errors_in("emit_signal(\"hit\", 1)"), "\"hit\" is not pub, so the engine cannot find it by name; use the signal itself, as hit."));
}

TEST_CASE("a literal member name is dynamic on Object, in a query, and for a property the engine lists per instance") {
	CHECK(errors_in("any.set_deferred(\"anything\", 1)").empty());
	CHECK(errors_in("var found := other.has_method(\"take_damage\")").empty());
	CHECK(errors_in("other.set_deferred(\"metadata/kind\", 1)").empty());
	// Only the name a signal that is not pub is registered under starts with @.
	CHECK(contains(errors_in("any.emit_signal(\"@hit\", 1)"), "\"@hit\" is how the engine knows a signal that is not pub"));
}

TEST_CASE("a literal member name checks what the call gives the member") {
	CHECK(contains(errors_in("set_deferred(\"speed\", \"fast\")"), "\"speed\" is float, which takes no String."));
	CHECK(errors_in("tween.tween_property(self, \"position:x\", 2.0, 1.0)").empty());
	CHECK(contains(errors_in("tween.tween_property(self, \"position\", 2.0, 1.0)"), "\"position\" is Vector2, which takes no float."));
	CHECK(contains(errors_in("tween.tween_property(self, \"position:z\", 2.0, 1.0)"), "\"position\" has no member \"z\"."));
	CHECK(contains(errors_in("tween.tween_property(other, \"speed\", 2.0, 1.0)"), "Node2D has no property \"speed\""));
	CHECK(contains(errors_in("call_deferred(\"reset\", 1)"), "\"reset\" takes 0 argument(s); 1 follow."));
	CHECK(contains(errors_in("emit_signal(\"done\", 1)"), "\"done\" takes 0 argument(s); 1 follow."));
	CHECK(errors_in("connect(\"done\", reset)").empty());
	CHECK(contains(errors_in("connect(\"done\", func(amount: int): pass)"), "passes 0 argument(s)"));
}

TEST_CASE("Callable(object, name) and Signal(object, name) take a literal name as a static reference, any other dynamically") {
	CHECK(errors_in("var c := Callable(self, \"reset\")\n\tc.call()\n\tvar s := Signal(self, \"done\")\n\ts.emit()").empty());
	CHECK(contains(errors_in("var c := Callable(self, \"missing\")"), "has no method \"missing\"."));
	CHECK(contains(errors_in("var s := Signal(self, \"hit\")"), "\"hit\" is not pub"));
	// Typed by the signal it names.
	CHECK(contains(errors_in("Signal(self, \"done\").emit(1)"), "argument"));
	// A name known only at run time (spec §10.4).
	CHECK(errors_in("var n := \"reset\"\n\tvar c: Callable = Callable(other, n)\n\tvar s: Signal = Signal(other, &\"visibility_changed\" if n.is_empty() else StringName(n))\n\ts.emit(1, 2)").empty());
	CHECK(contains(errors_in("var n := \"reset\"\n\tvar maybe: Node? = None\n\tvar c := Callable(maybe, n)"), "The value may be None (Node?); check it before taking its method."));
}
