extends RefCounted

func run(n: int) -> int:
	var node := Node3D.new()
	for i in n:
		node.position = Vector3(i, 2, 3)
	var result := int(node.position.x)
	node.free()
	return result
