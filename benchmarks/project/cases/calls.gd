extends RefCounted

func run(n: int) -> int:
	return fib(n)

func fib(k: int) -> int:
	if k < 2:
		return k
	return fib(k - 1) + fib(k - 2)
