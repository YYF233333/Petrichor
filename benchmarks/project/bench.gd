extends Node

const RUNS := 5

# Iterations and the group of each paired case.
const CASES := {
	"int_loop": [2_000_000, "compute"],
	"float_loop": [2_000_000, "compute"],
	"calls": [27, "compute"],
	"members": [2_000_000, "compute"],
	"vectors": [1_000_000, "compute"],
	"arrays": [1_000_000, "compute"],
	"property_get": [200_000, "boundary"],
	"property_set": [200_000, "boundary"],
	"engine_method": [200_000, "boundary"],
	"callable": [200_000, "boundary"],
	"signals": [100_000, "boundary"],
	"objects": [50_000, "allocation"],
	"closures": [100_000, "allocation"],
	"coroutines": [20_000, "allocation"],
}


func _measure(instance: Object, argument: int) -> Dictionary:
	var start := Time.get_ticks_usec()
	var result: Variant = instance.run(argument)
	return { "usec": Time.get_ticks_usec() - start, "result": result }


func _ready() -> void:
	var groups := OS.get_cmdline_user_args()
	for case: String in CASES:
		if not groups.is_empty() and CASES[case][1] not in groups:
			continue
		var instances := [load("res://cases/%s.gd" % case).new(), load("res://cases/%s.pet" % case).new()]
		var samples := [[], []]
		var results := [[], []]
		# One warm-up pair, then alternate which language runs first.
		for sample in RUNS + 1:
			for offset in 2:
				var language := (sample + offset) % 2
				var measured := _measure(instances[language], CASES[case][0])
				results[language].append(measured.result)
				if sample > 0:
					samples[language].append(measured.usec)
		var same := true
		for language in 2:
			for result: Variant in results[language]:
				same = same and result == results[0][0]
		print("bench: " + JSON.stringify({
			"case": case, "group": CASES[case][1], "iterations": CASES[case][0],
			"gdscript_usec": samples[0], "petrichor_usec": samples[1],
			"results": results, "same": same,
		}))
	get_tree().quit()
