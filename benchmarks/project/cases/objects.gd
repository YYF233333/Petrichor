extends RefCounted

func run(n: int) -> int:
	var total := 0
	for i in n:
		var node := Node.new()
		total += 1
		node.free()
	return total
