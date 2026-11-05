extends RefCounted

func run(n: int) -> int:
	var node := Node3D.new()
	for i in n:
		node.translate(Vector3(1, 0, 0))
	var result := int(node.position.x)
	node.free()
	return result
