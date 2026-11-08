# A script whose _set and _get take an engine property, as Dialogic's name
# label does with text (spec §10.5), driven by language_test.gd.
extends Label

var hooked := []

func _set(property, value):
	if property == "text":
		hooked.append(value)
		return true
	return false

func _get(property):
	if property == "text":
		return "from _get"
	return null
