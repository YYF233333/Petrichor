extends Node

# Runs each case of cases/, written in GDScript and in this language with the
# same algorithm, and prints the best of several timed runs (architecture §4).
# The results must be equal, or the two do not compute the same thing.

const RUNS := 5

# The argument of run() for each case.
const CASES := {
	"int_loop": 2_000_000,
	"float_loop": 2_000_000,
	"calls": 27,
	"members": 2_000_000,
	"vectors": 1_000_000,
	"arrays": 1_000_000,
}


# The first run warms up and is not counted.
func _measure(instance: Object, argument: int) -> Dictionary:
	var best := INF
	var result: Variant
	for i in RUNS + 1:
		var start := Time.get_ticks_usec()
		result = instance.run(argument)
		var elapsed := Time.get_ticks_usec() - start
		if i > 0:
			best = minf(best, elapsed)
	return { "usec": int(best), "result": result }


func _ready() -> void:
	for case: String in CASES:
		var gdscript := _measure(load("res://cases/%s.gd" % case).new(), CASES[case])
		var petrichor := _measure(load("res://cases/%s.pet" % case).new(), CASES[case])
		print("bench: %s %d %d %s" % [case, gdscript.usec, petrichor.usec, gdscript.result == petrichor.result])
	get_tree().quit()
