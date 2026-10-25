extends Node


func twice(value: Variant) -> Variant:
	await get_tree().process_frame
	await get_tree().process_frame
	return value


func assign_cancelled(greeter: Node) -> bool:
	var transform: Transform3D = await greeter.echo_later(null, 1)
	return transform.is_finite()
