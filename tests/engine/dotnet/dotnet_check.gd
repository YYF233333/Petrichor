extends Node

# The main scene of the dotnet group of run.py: classes written in C# beside
# those of this language (spec §11.1, §11.3).

const ErrorLog := preload("res://error_log.gd")

var failures := 0
var errors := ErrorLog.new()


func _check(condition: bool, what: String) -> void:
	if condition:
		print("dotnet: ok: ", what)
	else:
		failures += 1
		printerr("dotnet: FAILED: ", what)


func _check_errors(expected: Array[String], what: String) -> void:
	var mismatch := errors.compare(expected, "dotnet")
	_check(mismatch.is_empty(), what if mismatch.is_empty() else "%s: %s" % [what, mismatch])


func _ready() -> void:
	OS.add_logger(errors)
	var view: CsView = CsView.new()
	add_child(view)
	_check(view.made() == 1, "new() of a C# class runs its constructor")
	var counter = load("res://Counter.cs").new()
	var special = load("res://Special.cs").new()
	_check(view.counted(counter) == [3, 3, 30], "members, methods and signals of a C# class keep their value types")
	_check(view.counted_special(special) == [3, 3, 30], "a C# subclass is assignable to its C# base")
	_check(view.target_of(counter) == "None", "an object member of a C# class is T?, None when unset")
	counter.Target = special
	special.name = "Twin"
	_check(view.target_of(counter) == "Twin", "an object member of a C# class holds the object")
	_check(view.twin_of(counter, false) == "None" and view.twin_of(counter, true) == "Special 1", "an object result of a C# method is T?, and as narrows it to a C# class")
	_check(view.kinds(counter) == [true, false, false] and view.kinds(special) == [true, true, true] and view.kinds(view) == [false, false, false], "is and as with C# classes and their subclasses")
	counter.Anything = "any"
	_check(view.anything_of(counter) == "String any", "an untyped member of a C# class is Variant")
	counter.free()
	special.free()

	var messenger: Messenger = Messenger.new()
	var caller = load("res://CsCaller.cs").new()
	_check(caller.Found(messenger) == [true, false, 1, null, true, false], "C# finds only pub members by name")
	_check(caller.Raised(messenger) == 5 and messenger.level == 5, "C# writes a pub member by name")
	_check(caller.Take(messenger, {"damage": 3, "source": "cs"}) == 6, "a C# Dictionary decodes into a struct argument")
	_check(caller.Made(messenger) == {"damage": 4, "source": "pet"}, "a struct result reaches C# as a Dictionary keyed by field name")
	_check(caller.Heard(messenger) == {"damage": 6, "source": "pet"}, "a struct reaches a C# handler of a pub signal as a Dictionary")
	var missing = caller.Take(messenger, {"damage": 3})
	var mistyped = caller.Take(messenger, {"damage": "3", "source": "cs"})
	_check_errors(["messenger.pet:16: Argument \"hit\" of take(): Messenger.Hit.source is missing.", "messenger.pet:16: Argument \"hit\" of take(): Messenger.Hit.damage: expected int, found String."], "the program errors name the field of a C# Dictionary that does not decode")
	_check(missing == 0 and mistyped == 0, "a C# Dictionary without a field or with a field of another type is refused")
	messenger.free()

	print("dotnet: %d failure(s)" % failures)
	get_tree().quit()
