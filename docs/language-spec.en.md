# Petrichor Language Specification (Draft)

**v0.1 · 2026-10-21 · Public draft for comment** · 中文：[language-spec.zh.md](language-spec.zh.md)

This document specifies a statically typed scripting language for Godot. Petrichor is the project codename. This draft has Chinese and English versions with the same content.

## 1. Overview

### 1.1 Scope

- A statically typed scripting language for Godot with type inference.
- It reuses Godot core's basic types and infrastructure. To the language these are external types; the engine defines how they live, how they are copied, and their ABI contracts.
- Reusing Godot's semantics does not imply source compatibility with GDScript.
- It is delivered as an out-of-tree Godot module, without requiring a GDExtension compatibility layer or changes to core. Writing scripts does not require users to install a toolchain.
- There is no hot reload and no migration of live state; stopping and re-running is the normal workflow.

### 1.2 Semantics and implementations

The specification defines program validity, types and observable behavior without requiring a particular inference or execution algorithm. External value representations, equality and serialization follow §12; the rules for `==` within the language are in §2.3.

Design rationale and implementation proposals are recorded separately in the non-normative [design notes](design-notes.en.md).

### 1.3 Conventions

- Rules defining valid programs and required behavior are normative. Explanations, design rationale, proposals and plans are non-normative; §15 and Appendix B are identified as such.
- The following status markers are used:
  - **[Open]**: syntax or semantics remain unspecified.
  - **[Known limitation]**: unsupported in the first version. A restriction on valid programs remains normative.
  - **[Later]**: a deferred proposal, outside the first-version contract.
- Except for forms explicitly specified here, concrete syntax in code examples is [Open].
- "Program error" refers to the class of runtime errors defined in §10.
- All engine behavior cited here was checked against Godot master (4.8-beta, commit `469659e`). Sources are listed in Appendix A. This is a verification baseline, not a supported-version declaration; version selection is recorded in the [project plan](project-plan.en.md).

## 2. Type system overview

1. Strong static typing, inferred by default. If a type cannot be inferred, it is an error; there is no silent fallback to `Variant`.
2. `Variant` is only for engine interoperation; it is not a top type within the language (§2.1).
3. Method calls and operators produce capability constraints. Traits give these constraints names so they can be reused (§4).
4. There are named enums with payloads and exhaustive pattern matching. Option and Result are built from the same mechanism (§6).
5. Conditions must be bool. There is no general truthiness.
6. Single inheritance with overriding; no multiple inheritance. An override must honor the parent method's contract and must not narrow the parameters it accepts. Engine callbacks get their signatures from the engine contract.
7. No user-defined overloading. Default parameters, generics and overriding are kept. Engine methods that have several call forms are adapted by the binding layer.
8. The surface syntax uses Python/GDScript-style indentation and colons.

### 2.1 The Variant type

`Variant` denotes a non-NIL engine Variant value; a value that may be NIL uses `Variant?`, that is, `Option[Variant]`. It is used at interoperation positions such as Callables with unknown signatures, metadata, untyped containers and reflection interfaces.

`Variant` is not a top type within the language. A language value converted to `Variant` uses its type's engine encoding (§12.1, §12.4) and does not retain its static type: numbers lose their source width, and an enum becomes its encoding. `Variant` does not convert implicitly to other types; obtaining a language value from it requires an explicit conversion that performs the entry check for the target type (§12.5, §12.6); the `T(v)` numeric conversion to a numeric target follows §7.3 and performs no range check. The same `Variant` value may pass the checks of several target types; the result is determined by the target type. Explicit conversion is written `v as T` (§2.2).

Converting an Object reference to `Variant` does not change the Object inheritance hierarchy or its ownership and liveness rules (§5). Assigning a `Variant` value to another `Variant` copies the contained value; it does not add another container layer. The need for heterogeneous values within the language is deferred to a later dyn trait (see the [design notes](design-notes.en.md#deferred-language-proposals)).

### 2.2 is and as

`x as T` checks whether x can be used as type T, and its result is `Option[T]`. The static type of x must be an object type, `Variant`, or an Option of one of these; if x is None, the result is None. Other source types are compile errors: numeric conversions use `T(x)` (§7.3), and conversions between enums and their encodings use explicit conversion (§12.5). In the first version, T cannot be a generic parameter.

- **Object source, class target T:** if T is a subclass of the source type, the actual object (native class and script class) is checked at runtime; if it belongs to T the result is Some, otherwise None. The result refers to the same object as the original reference, with unchanged ownership (§5.1). If T is the source type itself or one of its ancestors, the result is always Some. If T and the source type are unrelated by inheritance, it is a compile error. If the source object has been freed, an invalid-access program error is reported rather than None (§6.5). The check holds only at the time it is made; later accesses still check liveness and script identity (§6.5).
- **`Variant` source:** the entry check for T is performed (§12.5, §12.6); on success the result is Some containing the decoded value, otherwise None. For example, an INT of 300 gives None for `as u8`, whereas the numeric conversion `u8(v)` gives 44 under §7.3. The same value may pass the checks of several target types; for example, an enum encoding can pass `as Array` and may also pass `as Cmd`. Container element types are not checked by the conversion; elements are checked when read (§12.2).

`x is T` is a bool equivalent to `(x as T) != None`. It does not narrow the type of x (§6.3); when the converted value is needed, write `if var Some(e) = x as T:`. A conversion that must succeed is written `(x as T)!`; when `!` applied directly to an `as` expression fails, the diagnostic names the actual and expected types. Operator precedence of `as` and `is` is [Open]. The node lookup `$X` checks the node's class against the expected type (§6.5).

```
@onready var sprite: Sprite2D = $Sprite2D        # must succeed
var e = (scene.instantiate() as Enemy)!           # must succeed
for body in get_overlapping_bodies():
    (body as Enemy)?.take_damage(1)               # skipped unless an Enemy
    if var Some(p) = body as Player:
        p.hit()
var hp = get_meta("hp") as i32                    # i32?: None if not an integer or out of range
```

### 2.3 Equality

The meaning of `==` is determined by the static types of both operands, and `!=` is its negation. Users cannot define their own `==` (item 7 of §2), so classes compare only by identity.

| Operand types | Rule |
|---|---|
| Numbers | Compared in the common type of §7.5; if no common type exists, it is a compile error, for example `i64 == u64` or `i64 == f64`. Floats follow IEEE 754: NaN is not equal to NaN, and 0.0 equals −0.0 |
| bool, String, StringName, core value types | Both operands must have the same type; the engine's comparison for that type is used; String and StringName compare by content (§7.7) |
| Objects | Compared by identity; the two types must be related by inheritance, otherwise it is a compile error. Comparison is not an access and does not check liveness |
| `Option[T]` | None equals only None; `Some(a) == Some(b)` exactly when `a == b`. When one operand is `Option[T]` and the other is T, the T is wrapped in Some before comparing |
| User enums | Same variant, and fields pairwise equal under their types; entirely payload-free closed enums compare members, with same-number aliases equal; open enums compare integer values. Comparing an enum with an integer is a compile error |
| `Array[T]`, Dictionary | The engine's container comparison is used (§12.2) |
| `Variant` | The engine's Variant comparison is used, comparing INT and FLOAT numerically; when the other operand has another type, it is first encoded under §12.1 |
| Callable, Signal, RID | The engine's comparison is used |
| `weak[T]` | Not provided (§5.2) |

In the first version, `<`, `<=`, `>` and `>=` are provided only for numbers and String; numbers compare in the common type of §7.5, and a missing common type is a compile error.

### 2.4 for loops

In `for x in e`, e may be an integer range `range(n)`, `range(a, b)` or `range(a, b, step)`, or an `Array[T]`, a `Dictionary[K, V]`, a packed array or a String. Iterating an integer directly (as in `for i in 5`), a float or a `Variant` is a compile error; a `Variant` must be converted first, for example `(v as Array)!` (§2.2). [Known limitation] The first version does not support user-defined iterator protocols.

| Iterated value | Type of the loop variable |
|---|---|
| `range(…)` | The common type of the arguments under §7.5; i32 if all arguments are literals; a compile error if no common type exists |
| `Array[T]` | T; each element read is checked (§12.2) |
| `Dictionary[K, V]` | The key type K; values are read with `d[k]` |
| Packed arrays | The element type declared by the engine, for example u8 for PackedByteArray, i32 for PackedInt32Array, f32 for PackedFloat32Array and String for PackedStringArray (Appendix A) |
| String | String, one character at a time |

The loop variable may be annotated, for example `for i: i64 in range(n)`. The annotation provides the expected type for flexible literals in `range`, and other arguments are converted to the annotated type at check boundaries. When iterating a container, the annotation must be the element type or an ancestor of it; downcasts use `as`.

The arguments of `range` are evaluated once; a step of 0 is a program error, and the step may be negative. Incrementing the loop variable does not overflow at the type's limit.

Modifying a container during iteration follows engine behavior and is not undefined behavior: an Array is iterated by index and its length is rechecked at each step, and iterating a Dictionary ends when the current key is erased (Appendix A). Element reads are still checked under §12.2. Each iteration uses a fresh loop variable, and closures capture by value (§5.4); reassigning the loop variable does not affect later iterations.

## 3. Type inference

Inference is defined by rules R1–R7. Under these rules, an implementation needs only local inference within each function, plus joint solving of return types and members across declarations (§11). It needs no let-polymorphism, no value restriction and no anonymous structural constraints.

### R1 Types are not determined by callers

The type of a declaration is determined only by the declaration itself and its body. With mutual recursion, it is determined together with the strongly connected component (SCC) it belongs to. Call sites play no part.

Rationale: if types were inferred from callers, a function's type would change every time someone adds a call. In addition, some callers cannot be seen by static analysis at all, such as `call()`, signals connected in the editor, and AnimationPlayer method tracks.

Exception: a lambda's parameter types may come from the expected type at the place where the lambda is written. This is purely local and does not violate R1.

### R2 Anything the engine can call is monomorphic; generics must be explicit

Class methods, signals and member properties are all entry points for the engine. `call()`, `has_method`, signal connections, `call_deferred`, Tween and RPC all look them up by name and pass Variant arguments. They must therefore have concrete signatures. A generic function must declare its type parameters explicitly, e.g. `func max[T: Ord](a: T, b: T) -> T`, and generic functions are not exposed to the engine.

Rationale:

- With implicit generalization, the static path and the `call()` dynamic path of the same call could produce different results. The static path instantiates with i32, while the dynamic path only has the 64-bit Variant int.
- If inference decided whether a function is generic, editing a function body could make a method disappear from `has_method`. A signal already connected in the editor would then fail to find its method at runtime.

### R3 Parameter types

A parameter's type is determined as follows:

1. If the method overrides an engine virtual function, the type comes from the engine contract.
2. Otherwise, look at the **positions with known types that the parameter flows directly into**: passed as an argument to a parameter of known type; assigned to a variable or member of known type; one operand of a binary operator whose other operand has a known type (the parameter then takes that same type); returned from a function with an annotated return type. If all such positions have the same type, that is the parameter's type.
3. Otherwise, an annotation is required.

Two things are not evidence:

- **Method calls.** `x.hit()` tells you nothing about x's type. A method with that name could belong to any class, and adding a new class should not change the meaning of existing code.
- **Flowing into a Variant or Variant? position.** `print(x)` does not determine x's type as `Variant`; otherwise it would be a silent fallback to a dynamic type.

```
func take_damage(amount):     # hp is i32; hp -= amount → amount: i32
    hp -= amount
func _on_body_entered(body):  # only body.hit() is called → annotation required
    body.hit()
```

### R4 Local variables

A local variable's type is solved over the whole function body and then fixed.

- If the initializer has a concrete type, that type is final. Later assignments do not widen it; implicit conversions follow §7.3.
- The **open parts** of an initializer are flexible literals (§7.2) and the element type of an empty container. They are determined as follows:
  1. **Inflows first.** If values of known type flow into the variable (assignments, the right-hand side of compound assignments, elements written into an empty container), the type is the lossless common type of the initializer and all those values. For numbers, the common type follows §7.5, and if none exists an annotation is required. Non-numeric types must be identical; no common ancestor class is computed. An inflow made up only of the variable itself and flexible literals carries no type information.
  2. **Otherwise, outflows.** If there are no such inflows, look at the positions with known types that the variable flows into (the same kinds of positions as in R3). If they all agree, use that type. If they disagree, or there are none, a flexible literal takes its default type (i32 for integers, f64 for floats), and an empty container's element type requires an annotation.
  3. When two open locals are assigned to each other, they form a group that shares one type, determined from the inflows and outflows of the whole group.
  4. The result does not depend on the order in which the constraints appear.
- A local without an initializer must have a type annotation; its type is the annotated type, and it takes no implicit default. At every read, every control-flow path reaching that point must have assigned it. Conditions are treated as possibly true or false, except constant `true`/`false` conditions, and an exhaustive match covers all cases. A closure cannot capture a local that is not yet definitely assigned.

```
var ratio = 0
ratio = position / length    # f64 flows in → ratio: f64; the literal 0 becomes f64

var t = 0.0
t += delta                   # f64 flows in → t: f64
sprite.modulate.a = t        # flows out to f32: precision loss, no warning by default

var groups = 1
dispatch(list, x, y, groups) # nothing flows in; all outflows are u32 → groups: u32
```

These rules are based on a survey of real Godot projects; see the [survey report](surveys/literal-local-constraints.en.md). The report records the warning defaults used in that survey; current defaults are specified in §7.3.

**Members are resolved in source order.** When you call a method or access a member on a value whose type is not yet determined, the constraints appearing before that point must determine the type under the rules above. Otherwise, it is an error asking for an annotation. If the value is a flexible literal with no constraints before that point, it takes its default type right there, and any later constraints become ordinary implicit conversions. This is the only case in R4 where the order of the code matters.

```
var a = 1
a.abs()             # no constraints before this point → a: i32
a = big_i64         # i64→i32: truncation; optional integer-conversion warning

var items = []
items[0].hp -= 1    # element type unknown here → error, even though append(enemy) follows
items.append(enemy)
```

### R5 Return types

The return type is inferred from every return in the function. When different branches return different types:

- different variants of the same enum merge into that enum;
- different numeric types merge into their common type per §7.5;
- different classes are not merged into a common ancestor. The first version reports an error and asks for an annotation, because the inferred ancestor is usually just Node or Object, and it would change whenever the class hierarchy changes;
- returning, say, an integer in one branch and a string in another does not produce an anonymous sum type.

### R6 Members and signals

A member variable's type comes from its annotation or its initializer. A member initialized with an empty container must be annotated; its type is not inferred from how other methods or other files use it, because the Inspector, `get_property_list` and inference in other files all need the member's type up front. Signal parameters must be annotated as well, because emit sites are callers (R1). Rules for emitting, connecting and awaiting signals are in §9.

### R7 Inferred signatures are the interface

Editing a function body can change its inferred signature and make calls in other files fail. This is the inherent cost of an inference-first design. When it happens, the diagnostic must show three things: the type the caller expected, the type inferred for the callee, and the source line that determined it. Being a coroutine is likewise part of the inferred signature (§9): adding an await to a function makes callers that use its call as a value fail, and the diagnostic must name the await line that made it a coroutine.

### 3.8 Where implicit conversions happen

Implicit numeric conversions (§7.3), implicit conversions between string types (§7.7) and conversion from a subclass to its superclass happen only at checking boundaries. These are assignments, arguments and return values whose expected type is known, and binary operators whose operands both already have concrete types. If a type is not yet determined, the check waits until it is.

## 4. Generics and traits

- A trait names a capability constraint explicitly. Whether a type satisfies a trait is decided structurally: a type with methods of matching signatures satisfies it, without having to declare so.
- Generic functions are the only source of polymorphism (R2). Inside a generic function, every method call and operator applied to a type parameter must be covered by a trait bound or a built-in operator constraint. Operators in generic code require both operands to have the same type (`T + T -> T`).
- In the first version, traits contain only method signatures and can be used only as bounds on generic functions. There are no default methods, associated types, generic traits or trait objects.
- Traits are not a back door for field injection, mixins or multiple inheritance, and they do not change the Node type tree.
- **Generic code must behave exactly like the same code written for a concrete type.** It must not fall back to Variant operations. For example, `add[T](a: T, b: T)` instantiated with i32 must follow 32-bit arithmetic and overflow rules, while a Variant int is 64-bit and would give different results. External types also copy and destroy differently: String is copy-on-write, and Ref is reference-counted.
- For each concrete instantiation, trait and built-in operator constraints resolve at compile time to a single implementation. The representation of type parameters and the execution strategy are not prescribed here; see the non-normative [design notes](design-notes.en.md#generics-and-execution).

## 5. Object model and lifetimes

### 5.1 Kinds of objects

Every class instance is a Godot Object with a ScriptInstance attached. A class without `extends` inherits RefCounted, as in GDScript, and the language's reference count is the engine's reference count. Semantics depend on the static type:

| Kind | Static type | Semantics |
|---|---|---|
| Owned reference | RefCounted and its subclasses (including Resource, and classes without `extends`) | A strong reference keeps the object alive; `weak` is available |
| Handle | Classes that do not derive from RefCounted (e.g. Node), except Object itself | A reference does not keep the object alive; it is essentially an ObjectID plus a liveness check. `weak` is meaningless and not allowed |
| Decided by the actual object | Object | A strong reference if the actual object is RefCounted, otherwise a handle; `weak` is available |
| Value | Numbers, bool, core value types such as Vector2, String, enums | Assignment copies (copy-on-write and similar optimizations come from the engine contract) |

**Changing the static type never drops ownership.** A RefCounted object upcast to Object is still held by a strong reference, and it stays that way when downcast back. This matches how Variant holds objects. Object is RefCounted's only ancestor, so Object is the only static type where the actual object decides the semantics. The same ownership rule applies when generic code is instantiated with Object.

### 5.2 Reference cycles and Node lifetimes

- There is no tracing GC, and no move or borrow annotations.
- Reference cycles are not collected automatically. Break them with weak back-references, or manually at an explicit point in the object's lifetime. Do not count on destructors to break a cycle, because the cycle itself keeps the destructors from running.
- Node lifetimes are managed by the engine; references from the language never keep a Node alive.

`weak[T]` provides `get() → Option[T]`: Some when the object is still alive, None after it has been freed. T may be RefCounted, a subclass of RefCounted or Object. A statically known non-RefCounted type such as Node cannot use weak (§5.1).

When the actual object is RefCounted, a successfully obtained value holds a strong reference. Bound to a local, that value keeps the object alive under §5.1. When T is Object and the actual object is a non-RefCounted object such as Node, the obtained value remains a handle: it does not extend the object's lifetime, and accesses still check liveness.

Obtain the result with one get, then obtain the object with conditional pattern binding under §6.3, for example `if var Some(p) = parent.get():`. Checking a weak reference and separately obtaining its value later does not provide the guarantee of a single acquisition. This interface follows the engine's threading contract; it does not guarantee safe acquisition under arbitrary concurrent access. Reference release and threading boundaries are specified in §5.5.

`weak[T]` is a value type that cannot be modified in place. Its default is the empty weak, whose `get()` returns None (§8.4). `weak[T]` cannot be used with `@export`, and the first version provides no `==` for it. At the engine boundary, `weak[T]` is encoded as an engine WeakRef object, and the empty weak as a WeakRef that refers to no object (§12.3); each engine read of a weak member is not guaranteed to yield the same WeakRef object. If `get()` finds the target alive but not a T, it is a program error (§12.3).

### 5.3 External containers and write-back

- Array and Dictionary are shared references.
- A Packed*Array is shared by reference between script variables, but built-in engine properties and methods return copies of it. That is why `line.points.append(p)` silently does nothing in GDScript.
- **Write-back rule:** modifying a value-type obtained from a property performs "get, modify, set". This includes calling a method that modifies the value. For example, `node.position.x += 1` and `line.points.append(p)` both write the result back to the property. Whether a method modifies its receiver is determined from the builtin method's const information.

### 5.4 Copying and closures

- Assignment copies a value or shares a reference according to the type (§5.1). Shallow-copying or deep-copying an object requires an explicit call.
- Copying uses each type's existing APIs; there is no separate uniform copy/deepcopy syntax. Array, Dictionary, Resource, Node and similar types use their respective `duplicate`-style interfaces. The interface contract determines the scope, options and sharing behavior of a copy; copying the entire object graph deeply is not guaranteed.
- User-defined classes may use inherited copying interfaces, including `duplicate` inherited by Resource and Node subclasses. When no suitable interface exists, the user writes a constructor or copying method. Copy operations are not generated automatically.
- Enums are value types: assignment copies their value. Reference payloads remain shared according to their types; copying an enum value does not deep-copy them.
- Closures capture current values by value. A captured reference still points to the same object. A closure cannot rebind an outer local variable. Strong reference cycles created through closures must also be broken manually.

### 5.5 Reference release and threading boundaries

When leaving a scope, returning or exiting the current execution because of a program error, release the strong references whose ownership ends with that exit. Whether the actual object is destroyed still depends on other references and the engine contract. Values returned, captured by closures or stored elsewhere remain alive under their normal ownership relationships; leaving the original scope does not release those other owners' references. Suspending at await is not a scope exit, and references held by the suspended execution remain according to its lifetime. A suspended execution holds no strong reference to self; to keep a RefCounted instance alive while suspended, bind self to a local so that the suspended frame holds it. When a suspended execution is cancelled (§9), the references it holds are released as on a scope exit.

No destruction order is specified for multiple independent objects. A program that requires an order must perform the corresponding operations explicitly; no precise member-destruction order is specified. These cleanup rules apply to exits controlled by the language, not a guarantee of cleanup after a process crash or forced termination.

Node release and the threading restrictions of each engine API follow the engine contract. Using Variant or holding a strong reference does not automatically make an object or operation thread-safe. The first version provides neither its own thread scheduling nor automatic cross-thread release. Static-value cleanup must still keep the required script code and runtime available (§8.5).

## 6. Enums, Option and nullability

### 6.1 Enums

- An enum has value semantics. Its encoding at an engine boundary is specified in §12.4, and conversion to `Variant` uses the same encoding. The encoding does not change its value semantics.
- An enum's internal value representation is not its engine encoding. The encoding is a representation that can be explicitly copied out; constructing an internal value from an encoding requires target-type validation (§12.5).
- **Enum values cannot be modified in place;** you construct a new value instead. Reference-type payloads are not deep-copied when an enum is copied.
- **[Known limitation] The first version does not support recursive enums.** An enum's value layout must not contain a cycle, including cycles through value types such as Option (e.g. `enum Tree: Node(i32, Tree?, Tree?)`). A cycle is a compile error. The diagnostic shows the cycle and suggests either using a class or recursing through Array or Dictionary. Recursion through those reference types is allowed, e.g. `enum Json: …, Arr(Array[Json]), Obj(Dictionary[String, Json])`.

User-defined enums and imported enums not marked as open are closed types. A legal value must correspond to a declared variant. An undeclared integer cannot circulate as a legal enum value; exhaustive matching therefore covers all possible variants.

Among imported engine enums, bitfield-marked enums and enums whose values are verified not to be limited to their named members (§7.6) are **open enums**:

- Their value domain is every i64 integer, encoded as INT; named members are constants of the type.
- `E(n)` accepts any integer value representable in i64 without checking named members; `try_from_int` is not provided. Conversion to an integer uses the explicit integer access below.
- A match must have a catch-all branch; no exhaustiveness check is performed.
- A bitfield-marked open enum supports `|`, `&`, `^` and `~` between values of the same type, producing that type; these operations are not provided between different types.
- The default value is 0 (§8.4).

An enum whose variants are all payload-free retains its type identity and provides explicit integer access: `T(value)` with an integer target first obtains its i64 integer encoding, then applies the integer conversion rules in §7.3. Apart from the bit operations of open bitfield enums above, arithmetic, bit combinations, shifts and masks operate in integer types without producing undeclared enum variants. Access does not silently narrow the value or bypass constant-range rules. INT encoding at an engine boundary does not make the enum an integer type; converting it to `Variant` yields that INT encoding, and recovering a closed enum checks named members (§12.5).

An integer entering a closed enum must be checked against its legal named members. Failure must not reinterpret it as an enum, create an unknown variant or cause undefined behavior. A payload-free closed enum E provides `E.try_from_int(n) → Option[E]`: Some for a legal member, None otherwise. The checked cast `E(n) → E` is for conversions expected to succeed; an invalid runtime value is a program error. Both validate the original integer value, without first truncating or wrapping it into another member, and neither accepts non-integer input. A known invalid constant in `E(n)` may be diagnosed under the existing compile-time error rules. An invalid constant passed to the try method returns None as usual, without a compile error merely because failure is predictable. These forms do not apply to payload-bearing enums or object downcasts.

The integer encoding of an entirely payload-free enum is i64. This does not change the i32 default for ordinary unconstrained integer literals. Only entirely payload-free enums have numbers; explicit numbering in an enum with a payload-bearing variant is a compile error, because its encoding is name-tagged (§12.4) and it has no integer access. If the first member of an entirely payload-free user enum has no explicit number, it is 0; each later unnumbered member receives the preceding member's number plus 1. Explicit numbering is written `enum E: A, B = 4, C`, giving A, B and C the numbers 0, 4 and 5. A numbering expression must be a compile-time integer representable in i64. Implicit increment beyond the i64 range is a compile error, not wrapping. Imported engine enums retain their original numbers as recorded by the binding.

Names with the same number in one enum are aliases for the same value, not distinguishable variants. This applies to user and imported enums alike. Aliases add neither values nor branches required for exhaustiveness. Successful reverse conversion returns that value without promising to recover a unique declaration name.

### 6.2 `T?` is `Option[T]`

`Option[T]` has two variants: `None`, which represents absence, and `Some(T)`, which contains a value of T. `T?` is shorthand for `Option[T]`. There is no language-level `null` value.

Nesting is preserved. With `T = Enemy?`, `first[T](a: Array[T]) -> T?` distinguishes an empty array (`None`) from an array whose first element is absent (`Some(None)`). Neither value is `Some(Some(enemy))`.

Option has value semantics. Its internal layout is unspecified; its engine encoding is specified in §12.4: `None` is encoded as NIL, and when the payload encoding excludes NIL, `Some(x)` uses x's encoding directly. Bare NIL is not a valid `Variant` value, so `Variant?` represents `None` with NIL and `Some(v)` with v itself.

`Option[T]` converts only to `Variant?`, not to the non-null `Variant`.

### 6.3 Conditional pattern binding

The first version performs no type narrowing based on None checks: `x != None` is an ordinary bool expression and does not change x's type. A payload is obtained with conditional pattern binding, `?.` or `!` (§6.4).

The values of members, properties and container elements may be changed by other code. Read the value into a local, then use that local; even if external code later changes the original location, the use sees the value that was bound. Closures capture by value (§5.4) and cannot rebind the outer local.

Conditional pattern binding is written `if var PATTERN = expr:`. PATTERN uses match patterns, without special Option-unwrapping rules. The right-hand expression is evaluated once. On a match, pattern variables are bound in the successful branch's lexical scope; on a mismatch, the else branch, if present, executes. Bound variables may be reassigned under the existing var rules.

For example, `if var Some(x) = target:` matches the Some variant and binds its payload. `if var Ok(v) = result:` and user-defined enum variants follow the same rules. A Some pattern unwraps one layer only: when target is `Some(None)`, x is bound to the inner None.

Both refutable and irrefutable patterns are allowed. A pattern need not be refutable, and being certain to match does not introduce a warning. The bare-variable pattern `if var x = target:` binds the entire value and always succeeds, even when target is None. It neither implicitly unwraps Option nor tests truthiness. Its else branch never executes but still undergoes normal syntax and type checking. The successful branch retains its lexical scope, and the right-hand side is still evaluated once.

This form does not provide runtime `if const` or specify general destructuring assignment. The first version provides no binding declaration with an else block; see the [design notes](design-notes.en.md#deferred-language-proposals). Other concrete match-pattern syntax remains within the unspecified scope of §1.3.

### 6.4 Nullability sugar

- **Postfix `!`** extracts the value from `Some`. Applied to `None`, it is a program error, and the diagnostic gives the location. It guarantees presence, not that the object is still alive.
- **`?.`** is optional chaining. If the receiver is None, the whole chain short-circuits, and the result is nullable. `?.` is a single token; postfix `?` performs Result propagation (§10). The language has no `?:` ternary operator, so there is no ambiguity.
- `?.` does not treat a freed object as None; it still reports an invalid-access error.

### 6.5 Non-null does not mean alive

- A Node reference can be non-null even though the Node has already been freed, so invalid-access checks always apply. For example, if a non-null local Node variable is held across an await, it is still non-null afterwards, but the Node may no longer be alive.
- Being alive does not mean still being of that class either. The engine and other scripts can replace an object's script: the old script instance is deleted at once while the object itself stays alive (Appendix A). When accessing a member or calling a method through a reference whose static type is a language class T, the language checks liveness and also that the object's current script instance belongs to T or a subclass; otherwise it reports an invalid-access program error. This also applies to self: after its script has been replaced, a running method that accesses a member of self reports an error, while the method itself can still return; implementations must ensure that the storage of the deleted instance is not accessed. If the new script is a subclass of T, the check passes and accesses reach the new instance, initialized under the construction rules. References whose static types are native engine classes are unaffected. The language does not intercept writes to the `script` property (Appendix A).
- When an engine contract is unclear, the value is treated as nullable until there is evidence, and the local binding descriptions are then filled in.
- Scanning an external mutable container once does not give a permanent static guarantee about its contents (see §12.2 for how containers are checked).
- Absence checks on external values follow §6.3: bind the read value to a local first, then obtain the payload with conditional pattern binding.
- `$X` and `%X` are required node lookups. When the expected type (§3.8) is a subclass T of Node, the node found must belong to T; a missing node or a class mismatch is a program error. Without an expected type, the result is Node. An optional lookup uses an interface bound to return `Option[Node]`, followed by `as` when a subclass is needed (§2.2). Thus neither `@onready var child = $Child` nor `@onready var s: Sprite2D = $Sprite2D` needs an additional `!`. Non-null results still require liveness checks.

## 7. Numbers and strings

### 7.1 Types

| Item | Rule |
|---|---|
| Signed integers | i8, i16, i32, i64 |
| Unsigned integers | u8, u16, u32, u64 |
| Floating point | f32, f64 |
| int, float | Not types and not usable in type annotations; conversion constructors only: `int(x)` converts to i32 and `float(x)` to f64 (§7.3) |
| Numeric-literal type suffixes | Not provided in the first version; expected typing and defaults without context follow §7.2 |
| i128, usize, etc. | Not provided |

The first version provides no type suffixes for integer or floating-point literals, including `u`, `l`, `ll`, width suffixes and `f`/`d`. A numeric type can be selected with a type annotation, or a `T(x)` explicit numeric conversion in an expression (§7.3). The scientific-notation exponent markers `e`/`E` are not type suffixes and are unaffected by this restriction.

### 7.2 Flexible literals

- An integer literal takes the type of the other operand or the expected type. If its value does not fit in that type, it is a compile error. With no constraint at all, it is i32.
- A float literal takes the expected type, or f64 if there is no constraint.
- An integer literal may also take a floating-point type, e.g. `var v: f32 = 10` or `Vector2(1, 2)`.
- Plain string literals are flexible literals as well; see §7.7.

In `func f(x: u8): return x + 100`, the literal 100 becomes a u8 and so does the result. When x ≥ 156 the addition overflows, which is a program error (§7.4). In `Time.get_ticks_msec() - 1000`, 1000 becomes a u64.

### 7.3 Numeric conversions

Lossless numeric conversions happen automatically at checking boundaries (§3.8). Whether a conversion is lossless depends on the source type's full value range: i32→i64, u32→i64, i32→f64 and f32→f64 are lossless. Lossy implicit conversions are also permitted, subject to the constant rule below.

An explicit numeric conversion is written `T(x)`, where T is the target numeric type, for example `i64(x)` or `f32(x)`. The conversion constructors `int(x)` and `float(x)` are equivalent to `i32(x)` and `f64(x)` respectively; int and float are not types. If the static type of x is i64, u32, u64, an enum or Variant, `int(x)` is a compile error and must be written as `i32(x)` or `i64(x)`; the integer encoding of an enum is i64 (§6.1). In GDScript, `int(x)` is a 64-bit conversion; this restriction keeps ported code from being truncated without notice.

The target supplies the expected type for a flexible literal used directly as the operand (§7.2). Thus the literal in `u64(18446744073709551615)` takes u64 directly rather than first defaulting to i32. A value whose type is already determined follows the existing conversion rules of this section and §7.5. Explicit conversion does not exempt an out-of-range constant from a compile error.

```
var largest = u64(18446744073709551615) # u64, within range
var invalid = u8(300)                  # compile error
```

Object downcasts and checked conversions from `Variant` use `as` (§2.2). Other conversion forms are [Open]. This section does not specify `T(x)` construction for non-numeric types.

#### Integer conversions

For a non-constant integer value x converted to an N-bit integer type, let r be the unique integer with `0 ≤ r < 2^N` and `r ≡ x (mod 2^N)`. For an unsigned target, the result is r. For a signed target, the result is r if `r < 2^(N−1)`, and `r − 2^N` otherwise.

Thus narrowing discards high bits, and a same-width change of signedness preserves the bits. When widening, a signed source is sign-extended and an unsigned source is zero-extended, regardless of the target's signedness. These conversions have defined results even when they lose range. They are legal runtime operations and are independent of the arithmetic overflow setting.

| Source value and type | Target | Result |
|---|---|---|
| 300 as i32 | u8 | 44 |
| −1 as i8 | u8 | 255 |
| 255 as u8 | i8 | −1 |
| −1 as i8 | u16 | 65535 |
| 255 as u8 | i16 | 255 |

The table describes conversions of non-constant values. Converting a constant integer outside the target's range is a compile error, for implicit and explicit conversions alike. This error cannot be disabled. A constant within range is accepted unchanged.

```
const limit: i32 = 300
var small: u8 = limit         # compile error
var small_cast = u8(limit)    # compile error

func narrow(x: i32) -> u8:
    return x                 # defined conversion; 300 becomes 44 at runtime
```

The constant rule also applies when constant propagation establishes a single constant operand. A `var` declaration does not by itself prevent propagation. A range analysis showing only that a runtime value may lose range does not make that value a constant and cannot justify this error.

A compiler must diagnose out-of-range integer literals and out-of-range conversions in constant expressions. Further constant propagation may be incomplete in the first version and may improve in later versions, including across declarations or functions. An undiagnosed constant error remains an error; later diagnostics may reject it. Missing a diagnostic neither creates undefined behavior nor permits the optimizer to assume that the conversion is unreachable. If such a conversion executes, its result follows the integer conversion rule above.

#### Numeric values in Variant

A numeric conversion `T(v)` applied to a `Variant` value takes INT as an i64 value and FLOAT as an f64 value, then applies the numeric conversion rules of this section and §7.5. It does not require the requested type to match the original source width. A u64 converted to `Variant` is transported bitwise as INT (§12.1), so `u64(v)` recovers the original value. `T(v)` does not perform the range checks of an entry check; when a range check is needed, perform the entry check for the target type (§12.5, §12.6).

For example, an i32 value of 1 and an i64 value of 1 become the same INT when converted to `Variant`. In the following example x is a runtime input; if its value is 18446744073709551615, same has that value and signed is −1 under the integer conversion rule:

```
func convert_number(x: u64):
    var value: Variant = x
    var same: u64 = u64(value)
    var signed: i64 = i64(value)
```

This example describes runtime conversions, not a constant out-of-range conversion. Converting a value to `Variant` does not exempt it from the constant rule when constant propagation establishes the operand's value. A non-numeric `Variant` does not become numeric merely because a numeric target is requested; this is a program error.

#### Conversion warnings

| Warning class | Covers | Default |
|---|---|---|
| Precision loss | f64→f32; integer→float when precision may be lost (e.g. i32→f32, i64→f64) | Off |
| Integer conversion loss | Integer narrowing and changes of signedness that may lose range | Off |
| Float-to-integer range loss | Float→integer | On |

Each class can be set independently to off, warn or error. These settings do not disable constant errors. A constant exactly representable in the target type produces no loss warning. An explicit conversion accepts the corresponding loss, but does not exempt a constant from the integer range rule.

The engine mixes f32 and f64: `_process(delta)` receives a double, while Vector2 components are real_t (float in standard builds; Appendix A). With default settings, `position += velocity * delta` produces no warnings. Float conversions retain the rules in §7.5.

### 7.4 Integer overflow

- **Integer overflow is a program error**, in the same category as out-of-bounds access and integer division by zero.
- Runtime overflow detection is off by default. When it is off, the result of an overflow is well-defined: it wraps around at the bit width (two's complement). It is **not undefined behavior**. Optimizations must not assume overflow never happens, and all execution modes must produce identical results. Lockstep multiplayer and replays depend on this determinism.
- Even though the result is well-defined, overflow is still an error. The same program reports it when detection is on, and it is a compile error whenever the overflow can be proven.
- Explicit wrapping uses the integer methods `a.wrapping_add(b)`, `a.wrapping_sub(b)` and `a.wrapping_mul(b)` for addition, subtraction and multiplication respectively. No `&+` operator is added. The result retains receiver a's integer type and wraps at that type's bit width; common-type selection does not change the receiver's width.
- A flexible literal argument takes the receiver's type. An already typed argument is converted to the receiver's type under the existing call-conversion rules (§3.8, §7.3); explicit conversion is not always required. A constant argument outside the target's range remains a compile error. Explicit wrapping permits only overflow of the operation itself; that overflow is not an error during constant evaluation or with runtime overflow detection enabled. For example, `u8(255).wrapping_add(1)` produces u8 zero, while `u8(255).wrapping_add(300)` is a compile error because the argument is out of range.
- **Every overflow that can be proven at compile time is an error.** An expression that overflows every time it is evaluated is a compile error.
  - Every implementation must catch at least two cases: overflow while evaluating a constant expression, and a literal that does not fit its target type.
  - Detection of further cases is added over time. Each newly detected kind of case gets a conformance test, and detection is never removed. If a compiler upgrade makes old code fail to compile, that code was already wrong.
  - Proofs only look inside a single function. An overflow that can only be shown by reasoning across functions is at most a warning, never a compile error, so incremental compilation and error locations stay stable.
  - No false positives: an error is reported only when the expression overflows on every evaluation. Overflows that merely might happen belong to potential-risk warnings. The conditions for those are [Open], and the first version may issue none.
  - Within one version, editor diagnostics and export builds must use the same analyzer.

### 7.5 Mixed arithmetic, division and edge cases

**Mixed arithmetic:** once both operand types are determined, the result has the smallest common type that can represent the full value ranges of both source types without loss. Examples include i32+i64→i64, i32+u32→i64 and f32+f64→f64. If there is no such type, as with i64+u64, an explicit conversion is required.

The same lossless-common-type rule applies to integer/float arithmetic, without a separate float-priority rule. Flexible literals first take their type from context under §7.2; this rule then applies to the determined types. Whether a common type is lossless is decided from the source types, not the values in a particular evaluation. The lossy implicit conversions allowed at checking boundaries (§7.3) do not supply a common arithmetic type.

| Operand types | Common type |
|---|---|
| i16 and f32 | f32 |
| i32 and f32 | f64 |
| i32 and f64 | f64 |
| i64 and f32 or f64 | None; explicit conversion required |
| u64 and f32 or f64 | None; explicit conversion required |

**Floating-point arithmetic:** f32 and f64 arithmetic, rounding and non-finite results follow IEEE 754. Constant floating-point arithmetic follows the same rules; rounding or overflow to infinity does not invoke the integer constant-overflow errors of §7.4, and no floating-point overflow runtime check is added. Floating-point division by zero follows IEEE 754: a finite nonzero value divided by zero produces the appropriately signed infinity, while zero divided by zero produces NaN. These are not integer division-by-zero program errors.

This section adds no floating-point arithmetic warnings; additional floating-point diagnostics are deferred. The numeric-conversion warnings and their configuration already specified in §7.3 are unchanged. IEEE 754 conformance does not promise bitwise-identical platform math libraries or stable NaN payloads. Float-to-integer conversion still follows the language rule in the table below; saturation and NaN-to-zero are not requirements derived from IEEE 754 arithmetic.

| Case | Rule |
|---|---|
| Integer `/` | Truncates toward zero; there is no `//` operator |
| Integer division by zero (`/` and `%`) | Always a program error, regardless of the overflow detection setting |
| Sign of integer `%` | Same as the dividend |
| MIN / -1 | Counts as overflow: an error with detection on, MIN with it off. MIN % -1 is always 0 |
| Shift count negative or not less than the bit width | A program error: reported with detection on; with it off, the count is replaced by its non-negative remainder modulo the bit width. An out-of-range constant count is a compile error |
| Bits shifted out by a left shift | Discarded; not an overflow, regardless of the detection setting. For signed types the result is interpreted in two's complement |
| Right shift | Arithmetic (sign-extending) for signed types, logical for unsigned types; there is no `>>>` |
| Float→integer (explicit or implicit) | Out-of-range values saturate to the target's minimum or maximum; NaN becomes 0 |

In C++, an out-of-range float→integer `static_cast` is undefined behavior; the language does not copy that. This conversion rule does not change the floating-point arithmetic rules above.

**Shift typing:** shifts do not use the common-type rule above. The result has the type of the left operand; the right operand may be any integer type, and no common type is computed. A flexible literal as the left operand takes the expected type, or i32 if there is none; it does not take the right operand's type (an exception to §7.2). For example, `b << 4` with b of type u8 remains u8; `var m: i64 = 1 << 40` is valid, while in `var m = 1 << 40` the 1 is i32, the count is out of range, and a compile error is reported.

### 7.6 Engine boundary

- GDScript's default scalar widths are not inherited. Three notions of type are kept apart: the type in the language, the type in the engine API, and the ABI slot that Variant/ptrcall actually uses.
- Available scalar type information comes from `MethodBind::get_argument_meta()`, which returns `GodotTypeInfo::Metadata` values such as INT_IS_UINT64; extension_api_dump uses the same information for its `meta` field. This does not replace marking open enums based on the actual interface contract below.
- Imported enums are closed or open, as specified in §6.1. Bitfield-marked enums, meaning those whose ClassDB/GDType enum metadata carries the bitfield flag and whose property and argument information carries `PROPERTY_USAGE_CLASS_IS_BITFIELD`, are imported as open enums automatically, for example Control.SizeFlags and Mesh.ArrayFormat (Appendix A). If a verified interface contract shows that another enum's values are not limited to its named members, the binding description marks the whole type as open and records the interface positions and evidence. Key is an example: `key_label` can be any Unicode character other than control characters, and `get_*_with_modifiers` returns values with modifier bits added (Appendix A). All other imported enums remain closed. This neither erases all engine enums to integers nor requires callers to bypass bindings individually; combined values and bit patterns round-trip under the core interface contract, without rejecting legal combinations or silently losing bits.
- C++ operator overloads between different enum types are not imported with the bindings. For example, combining Key with KeyModifierMask is done in the integer domain and then converted back to the open enum. ClassDB/GDType enum metadata stores names, int64 values and a bitfield flag, not information that recovers the original declared width. Native enum width must not be inferred automatically from it (checked at Godot `469659e0617345149cd1eaec2e643d61d130aa6a`: `core/object/gdtype.h:48-52`, `core/object/class_db.h:488-495`; Appendix A).
- For a statically known u64 engine API slot, the ABI encoding between u64 and Variant int reinterprets the bits (§12.3); it is not a source-level constant conversion under §7.3. Converting u64 to `Variant` uses the same bit transport (§12.1). A real-world test case: the ObjectID of a RefCounted object has bit 63 set, so as a u64 it is at least 2⁶³, and it becomes negative when passed through a Variant.
- If an original unsigned engine number is exposed as a signed bit pattern, the binding preserves the recorded number and bit pattern under its verified transport contract. It must not silently change the number or introduce a u64 enum backing type on that basis.
- Runtime bounds checking is separate from overflow detection and is on by default. [Known limitation] The first version offers no way to turn bounds checking off and has no unsafe mode.

### 7.7 Strings

A plain string literal `"…"` is a flexible literal under the same rules as the numeric literals of §7.2: if the expected type is String, StringName or NodePath, it takes that type and is constructed at compile time; in a binary operation it takes the type of the other operand; with no constraint it is String. Locals are typed under R4; for example, `var action = "jump"` is a StringName if it flows only into StringName parameters. The prefixed forms `&"…"` and `^"…"` are fixed as StringName and NodePath respectively, and the path in `$X` or `%X` is a NodePath (§6.5).

At check boundaries (§3.8), String converts implicitly to StringName or NodePath, and StringName and NodePath convert implicitly to String, matching the conversions GDScript allows. Constructing a StringName from a String requires a locked lookup in a global table (Appendix A); literals are constructed at compile time and do not incur this runtime cost.

String and StringName can be compared by content with `==` and `!=` (§2.3). `<`, `<=`, `>` and `>=` apply only between Strings; StringName provides no ordering comparisons. Concatenating String and StringName produces a String.

```
Input.is_action_pressed("jump")        # the literal becomes a StringName
var action = "jump"                    # flows only into StringName parameters → StringName
anim.play("attack_" + str(n))          # String converted to StringName at the check boundary
get_node("UI/HealthBar")               # the literal becomes a NodePath
```

## 8. Variables, constants and initialization

### 8.1 var and const

- The keywords are `var` and `const`; there is no separate `let` or `mut`.
- `const` accepts only constant expressions. There are no constants that are computed at runtime and then frozen, no `@onready const`, and no freezing phase.
- A const declaration fixes its type from an explicit annotation or, without one, from its initializer. Remaining unconstrained integer literals default to i32 and floating-point literals to f64; the numeric-type and flexible-literal rules in §7.1–§7.2 still apply. Use sites do not infer the declaration's type.
- At a use site, constants follow the existing conversion and constant-range rules (§7.3). A fixed type does not discard the known value or disable constant propagation.

```
const N = 1              # i32
const SCALE = 0.5        # f64
const SMALL_SCALE: f32 = 0.5 # f32, from the annotation
const WIDE: i64 = 1      # i64, from the annotation
```

- Members, construction, scene injection and `@onready` all follow Godot's lifecycle. The language adds no user-facing types for tracking initialization state.
- A non-null member must be initialized from a valid source; `None` is not a value of T. Use `T?` for a value that may be absent.

Declarations without initializers are handled as follows:

| Declaration | Without an initializer |
|---|---|
| Member, static field | Requires a type annotation (R6). If T has a default (§8.4), it takes that default, created anew for each instance or class; otherwise it is uninitialized until its first assignment |
| `@export` member | As for members |
| `@onready` member | Compile error |
| Local | Requires a type annotation and definite assignment before any read (R4); takes no implicit default |
| const | Compile error |

- Uninitialized members include members and static fields without defaults in the table above, and `@onready` members whose initializers have not yet run. Reading one while uninitialized is a compile error when this can be decided statically; otherwise it is checked at runtime and reported with a diagnostic naming the member.
- An uninitialized member may be assigned after instantiation by its creator, by scene injection or by the class's own code; for example, a non-null `@export` reference with no supplied value may be supplied later. Every read while it remains uninitialized is checked under the preceding rule; an attempted read does not permanently disable the check. No instantiation-completion scan is added.
- The uninitialized state is one-way: once a member is initialized, neither the language's code nor an external write can return it to the uninitialized state. This state is neither a language-level null nor a default value of the type, and the language has no value denoting it. Explicitly writing NIL from outside into an initialized non-null member is rejected by the non-null entry check (§12.6). Successful initialization may be recorded, while ordinary external-value and object-liveness checks remain in force.
- When the engine or another script reads an uninitialized member through `get`, it obtains NIL; this is not a program error. This covers non-null exports with no supplied value and `@onready` members whose initializers have not yet run. The read check above applies only to reads by the language's own code. When the engine writes NIL, a member that is still uninitialized remains uninitialized; an initialized non-null member rejects the write and keeps its value. Consequently, `set(name, get(name))` does not change a member's state when the engine duplicates a node or resource or the debugger reads members (Appendix A).

### 8.2 Initialization order and rules

The language uses GDScript's initialization order:

- At construction, the initializers of all ordinary members run first, from base class to derived class, and then `_init` is called.
- `@onready` initializers run when `_ready` is called, before the body of the user's `_ready`, also from base class to derived class.

On top of that order:

1. **Construction-time member initializers must not reference self.** They may not call instance methods or read other non-const members. This is checked statically. It rules out a whole class of bugs where a base-class initializer makes a virtual call that reads a derived-class member that has not been initialized yet. **`@onready` initializers are exempt:** they run once the node is ready and may reference self, as in the common `@onready var child = $Child`.
2. **The `_init` of a Node or Resource subclass must be callable with no arguments.** Scene instantiation, resource loading and `duplicate()` all call `_init` without arguments. A violation is reported at compile time, or at instantiation time.
3. **Each `@onready` member has its own "initialized" flag, including members whose types have defaults.** The check is whether that member's initializer has finished, not whether `_ready` has started. Reading a member before its initialization has finished is a program error, and the diagnostic names the member and shows the call stack. This cannot be ruled out statically in general. Two examples:
   - A child's `_ready` runs before its parent's. If the child synchronously emits a signal, and the parent's handler reads one of the parent's `@onready` members, that member is not initialized yet.
   - An `@onready` initializer calls a method that reads another `@onready` member declared later, which is not initialized yet. If the initializer reads a later-declared member directly, this is reported at compile time.

### 8.3 Open questions

- The position of scene-property injection and entering the tree relative to the stages in §8.2, including their interaction with inheritance.

The initializer and `_init`/`_ready` ordering specified in §8.2 is a language rule. Checking engine compatibility with that ordering is a separate activity described in the [project plan](project-plan.en.md#compatibility-checks).

### 8.4 Default values

A default value is available only for the types specified below. Defaults are used for members and static fields declared without initializers (§8.1) and by `Array[T].resize(n)` (§12.2); locals take no implicit default. No object constructor is called implicitly to supply a default, and a closed enum's first variant is not selected automatically.

Core defaults below use the engine's default constructors, checked at Godot commit `469659e` (full revision `469659e0617345149cd1eaec2e643d61d130aa6a`); sources are also listed in Appendix A. They are semantic values, not a requirement to zero memory.

| Type | Default value | Rule or engine source |
|---|---|---|
| Integer and floating-point types (§7.1) | 0 | Language rule |
| bool | false | Language rule |
| String | Empty string | `core/string/ustring.h:281, 322-323, 678`; `core/templates/cowdata.h:88, 185, 233` |
| `Option[T]` | None, even when T has no default | Language rule |
| `Array[T]` | A fresh empty Array | `core/variant/array.cpp:45-51, 983-986` |
| `Dictionary[K, V]` | A fresh empty Dictionary | `core/variant/dictionary.cpp:43-50, 781-784` |
| Closed enums other than Option, including Result | No implicit default | Language rule; Option follows its own row |
| Open enums (§6.1) | 0 | Language rule |
| Variant | No implicit default | Non-null (§2.1) |
| Non-null Object references | No implicit default | Initialization rules (§8.1) |
| `weak[T]` | The empty weak; `get()` returns None | Language rule (§5.2) |
| Vector2, Vector2i, Vector3, Vector3i, Vector4, Vector4i | All components 0 | `core/math/vector2.h:54-58`; `vector2i.h:54-58`; `vector3.h:63-65`; `vector3i.h:56-58`; `vector4.h:51-54`; `vector4i.h:50-53` (all under `core/math/`) |
| Rect2, Rect2i | Zero position and size | `core/math/rect2.h:42-43, 401`; `rect2i.h:41-42, 238`; vector defaults above |
| AABB | Zero position and size | `core/math/aabb.h:45-46, 147` |
| Plane | Zero normal and distance d = 0 | `core/math/plane.h:42-43, 92` |
| Quaternion | (x, y, z, w) = (0, 0, 0, 1) | `core/math/quaternion.h:38-41, 125` |
| Color | (r, g, b, a) = (0, 0, 0, 1), opaque black | `core/math/color.h:39-42, 256` |
| Basis | Identity 3×3 basis | `core/math/basis.h:41-45, 247` |
| Transform2D | Identity 2×2 basis and zero origin | `core/math/transform_2d.h:58-62, 152` |
| Transform3D | Identity basis and zero origin | `core/math/transform_3d.h:43-44, 131`; Basis and Vector3 defaults above |
| Projection | Identity 4×4 matrix | `core/math/projection.h:55-60, 160` |
| StringName | Empty name | `core/string/string_name.h:65, 138-143, 183` |
| NodePath | Empty path | `core/string/node_path.h:54, 105`; `node_path.cpp:351-353` (under `core/string/`) |
| RID | Empty RID, ID 0; no valid resource handle | `core/templates/rid.h:40, 61-62` |
| Core Callable | Empty, invalid callable | `core/variant/callable.h:49-52, 135`; `callable.cpp:44-49, 151-156` (under `core/variant/`) |
| Signal | Empty, unbound signal | `core/variant/callable.h:179-184, 205`; `callable.cpp:532-545` |
| PackedByteArray, PackedInt32Array, PackedInt64Array, PackedFloat32Array, PackedFloat64Array, PackedStringArray, PackedVector2Array, PackedVector3Array, PackedColorArray, PackedVector4Array | Fresh empty packed array | `core/variant/variant.h:82-92`; `core/templates/vector.h:71, 318`; `cowdata.h:88, 185, 233` (under `core/templates/`) |

A default RID, core Callable or Signal is a legal value of its core type, but does not provide a usable resource, a callable target or a signal source. These empty engine values are not language-level null or Option.None. A default core Callable does not establish a statically known function signature (§9, §12.3). Likewise, the default Plane's zero normal does not establish a unit-normal precondition for a geometric operation.

Every default creation produces an independent mutable container, including each new element initialized by `resize(n)`. For example, separate default Arrays or Dictionaries do not share one mutable default instance. Ordinary assignment and an explicit `resize(n, fill)` retain the sharing rules in §5; fill is copied or shared according to its type, never automatically deep-copied.

For `Array[T]`, `resize(n)` is available only if T has a default; new elements receive T's default. Otherwise an explicit fill value is required (§12.2). `Option[T]` has a default independently of T, and empty container defaults do not require their element types to have defaults.

### 8.5 Static initialization

Static initialization belongs to class/script loading and dependency satisfaction. Each runtime class state is initialized once, not on every instantiation. Base-class static initialization completes first, followed by this class's static fields in declaration order, then this class's `_static_init`. Nested classes obey the same dependency constraints; nesting alone does not unconditionally place their initialization before or after the enclosing class.

Before a cross-class static-value read, a static-method call or construction of an instance, the target class's static initialization must have completed. Cycles in type references or type inference are not initialization-execution cycles. An unsatisfiable dependency in actual initialization execution is a program error whose diagnostic includes the dependency chain. Errors decidable statically are reported at compile time; others at runtime. Temporary zero values or other incomplete values do not substitute for a result, and the failure does not cause undefined behavior.

During a class's own initialization, its initializers and the internal code of that class they call may read its fields whose initialization has already completed in declaration order. Executing this internal code does not require completion of the whole class. This does not permit returning through another class to the unfinished class, constructing a target class whose initialization is incomplete, or reading an incomplete field to bypass the completion constraints.

A field with an explicit initializer cannot be read before that initializer completes, even if it is numeric or Option. A static field without an initializer follows §8.1: if its type has a default, it becomes readable when its default initialization completes at its position in declaration order (§8.4); otherwise it is uninitialized until its first assignment, and reading it before then is a program error rather than an initialization-dependency failure. Preparing underlying storage does not initialize the field. A default value also does not establish application preconditions such as resource-handle validity.

Declaration initialization and preparation of default storage do not invoke field setters. Setter rules for subsequent ordinary assignment are unchanged. In the first version, static initialization cannot suspend with await.

If initialization fails, the class cannot be used normally as a completely initialized class. Later access must not automatically retry initialization or its side effects. External side effects that have already occurred are not guaranteed to be rolled back. While static values are being cleared, the script code and runtime they depend on must remain available. These rules do not provide hot reload or live-state migration.

### 8.6 Properties: stored and computed

Properties are either stored or computed; the concrete syntax is [Open]. A setter takes the property type and returns void; a getter returns the property type.

| | Stored property | Computed property |
|---|---|---|
| Underlying storage | Yes; initialized under §8.1; may be `@export` | None; no initializer; cannot be `@export` |
| Setter | Optional | Optional; read-only without one |
| Getter | Not allowed; reads always read the field | Required |
| Property seen by the engine | With STORAGE; duplication and saving read and write only the field | Without STORAGE, so neither saved nor duplicated; usable by `get`, `set`, Tween and AnimationPlayer |

- **Stored properties:** reads by the language's code and by the engine read the field directly and run no user code. Writes go through the setter; inside that property's own setter, assignment writes the field directly. A compound assignment `hp += 1` reads the field and writes back through the setter; a partial modification of a value-type property (such as `pos.x += 1`) is written back through the setter under the write-back rule of §5.3. Member initializers, `@onready` initializers and static-field initializers write the field directly without calling the setter (§8.5). The engine read and write rules for uninitialized members in §8.1 apply to all stored properties.
- **Computed properties:** reads by the language's code and by the engine call the getter, and writes call the setter, also inside the class. The getter and setter cannot refer to the property itself. Assigning a read-only computed property from the language's code is a compile error, and writing it through an engine `set` is a program error. A getter that reads other uninitialized members is treated as a read by the language's code (§8.1).
- An engine `set` first performs the entry check for the property type (§12.6) and then calls the setter. If another method called from a setter assigns the property again, the setter is entered again.
- Setters and getters cannot be coroutines (§9). A side effect that must wait until the node is ready is written by checking `is_node_ready()` in the setter and applying it once in `_ready`; when a setter contains await, the diagnostic suggests this rewrite.

```
@export var color: Color:
    set(v):
        color = v
        if is_node_ready(): _apply_color()
func _ready():
    _apply_color()
```

Lazy initialization or caching uses a private stored field plus a computed property. This split means engine reads of stored data (duplication, saving, the debugger) run no user code, proxy and derived properties leave no useless storage, and read-only properties can be declared. In a survey of real projects, getters needing manual rewriting in GDScript code occur about 0.12 times per 1,000 lines, and await in setters about 0.19 times per 1,000 lines (see the [survey](surveys/property-accessors.en.md)).

## 9. await, signals and Callable

- await is integrated minimally with Godot's signals, timers, frame events and main loop. There is no separate executor. The language is responsible for suspending a function's frame and for saving and restoring its local variables.
- Lifetimes, resumption order and reentrancy follow GDScript as closely as possible. Apart from the cancellation on instance destruction below, which matches GDScript, the language does not add its own automatic deferral or cancellation mechanism. A signal can resume a suspended function synchronously, so code must not assume it continues only on the next frame.
- A suspended execution holds no strong reference to self (§5.5). When its instance is destroyed, the instance's suspended executions are cancelled: pending signal connections are disconnected, references held by the suspended frames are released under §5.5, and they are neither resumed nor reported as errors. This follows GDScript (Appendix A). Consequently, when awaiting a coroutine on another instance that is then destroyed, the awaiting execution stays suspended until its own instance is destroyed. A node leaving the tree is not destroyed, and its suspended executions are not cancelled for that reason.
- Functions, closures and callbacks whose signatures are known use static function types, so their arguments and results are checked at compile time.
- Engine Callables whose signatures are unknown keep a type-erased form. In the first version they use callv through the binding layer. Arguments use `Variant` or `Variant?` according to the binding contract; an unknown result is `Variant?` unless a reliable contract excludes NIL (§12.6). Option absence checks follow §6.3; after obtaining a `Variant`, concrete operations require an explicit conversion to the target type (§2.1).
- **Signal types.** Signal parameters must be annotated (R6); the static type of a signal member is a signal type with those parameter types, and its syntax is [Open]. The signal information visible to the engine includes the parameter types.
- **Emitting.** When emitting through a static signal (such as `hit.emit(5, self)`), arguments are checked statically against the parameter types, implicit conversions happen only at check boundaries (§3.8), and arguments are encoded by parameter type before being passed to the engine (§12.1). Emitting by name (`emit_signal("hit", …)`) is type-erased: arguments are `Variant` and are not checked statically. The engine does not check arguments against the declaration when emitting (Appendix A).
- **Connecting.** When connecting a Callable with a static function type (such as `hit.connect(on_hit)`), the parameter counts must be equal, and each signal parameter type must be assignable to the corresponding function parameter type; a function parameter may be an ancestor of the signal parameter type. Results of `bind` and `unbind`, and type-erased Callables, are checked when called. When the engine calls a receiver written in the language, arguments undergo the entry check for the receiver's parameter types (§12.6).
- **Result of awaiting a signal.** If a static signal has no parameters, await produces no value, and using its result is a compile error. With one parameter, the result is a value of that parameter's type; if the emitter passes a value that does not match it, an entry-check error is reported at the resumption point. With several parameters, await can only be a statement in the first version and its result cannot be used; in a corpus survey, code using such a result occurred once, and it involved a signal with unknown parameters (see the [survey](surveys/await-signal-results.en.md)). Signals of engine classes are typed from ClassDB argument information, and Object arguments are nullable when the contract is unclear (§6.5); for example, `await area.body_entered` yields `Node2D?`. Signals with unknown parameters keep GDScript's result shape with result type `Variant?`: None for no arguments, Some(the argument) for one, and Some(an Array of the arguments) for several (Appendix A).
- **Coroutines.** A function (including a lambda) whose body contains await is a coroutine. This is inferred from the body and is part of the inferred signature (R7).
- **Calling coroutines.** `await f()` waits for the coroutine to finish and yields its return value. Called as a statement without await, the coroutine runs until its first suspension, after which the caller continues and the return value is discarded. Using the value of a coroutine call in an expression without await is a compile error.
- **The operand of await** must be a signal or a coroutine call; using await on any other expression is a compile error, including a `Variant` value.
- **Overrides.** An override must have the same coroutine status as the base method. The exception is a base method returning void, including engine virtuals such as `_ready` and `_process`: the override may be a coroutine, and calls through the base type behave as statement calls. Overrides of engine virtuals that return non-void values cannot be coroutines.
- **Function types** record coroutine status; the syntax is [Open]. await can be used only for calls through a coroutine function type. A coroutine returning void can convert to the corresponding non-coroutine function type, with calls behaving as statement calls, so it can be connected to signals; a coroutine returning a non-void value cannot be converted this way.
- `_init`, `_static_init` and property setters and getters cannot be coroutines; static initialization cannot suspend (§8.5).
- **Engine calls to coroutines.** When the engine or another language calls a coroutine written in the language, the call returns NIL at the coroutine's first suspension and the return value is discarded. [Known limitation] The first version does not support cross-language coroutines: other languages cannot await the results of the language's coroutines, and the language cannot await other languages' coroutines or async values (see the [design notes](design-notes.en.md#deferred-language-proposals)).
- [Open] The following cases must be tested against the engine version selected for evaluation: the node's owner being destroyed, the node leaving the tree, the signal source being destroyed and the suspended frame being freed. Double resumption, leaks and invalid access must all be avoided.
- [Later] Variadic generics that keep full signatures (parameter packs). These are not a reason to adopt full row polymorphism.

## 10. Errors and diagnostics

Expected, recoverable failures are returned as Result values. A Result has a success type and an error type: Ok(v) contains a success value, and Err(e) contains an error value.

Postfix `?` applies only to a Result expression:

- For Ok(v), the expression yields v and evaluation continues.
- For Err(e), evaluation returns Err(e) immediately from the current function or closure.

The current function or closure must return Result with the same error type as the operand. Its success type may differ from the operand's success type. Error types must match; no implicit error-type conversion is performed. A `?` inside a closure returns from that closure, not from its enclosing function.

Option is not an operand of `?`. Optional chaining uses the separate token `?.` (§6.4).

- **Program errors** include out-of-bounds access; invalid access, including access to a freed object or one whose script was replaced (§6.5); reading an uninitialized value; integer division by zero; a failed non-null assertion; integer overflow and out-of-range shifts with detection on (§7.4–§7.5); a failed checked enum conversion (§6.1); a failed required `$X` lookup (§6.5); numeric conversion of a non-numeric `Variant` (§7.3); an unsatisfiable static-initialization dependency (§8.5); an encoding beyond the depth limit (§12.4); and a failed external-value check (§12.3, §12.5–§12.6). A program error produces a runtime diagnostic and a call stack. Where script execution stops is decided by the engine debugger; the whole game does not have to exit.
- A diagnostic identifies the source file, location and relevant chain of type constraints.
- An error in another file must not be reported only as "the entry script failed to load". A type conflict shows the types on both sides and the relevant source locations (R7).
- If compilation fails, no partially compiled signatures or executables are published.

## 11. Compilation model

Recursion, mutual recursion and inference across files are solved jointly for each strongly connected component (SCC). Components consist of declarations (functions or members), not whole files. Signatures exported across files may be inferred; public functions need not all have annotations.

A declaration's dependencies include methods selected using inferred receiver types. For example, in `func f(): return make_b().g()`, the dependency on `B.g` follows from `make_b` returning B. No particular dependency-discovery algorithm is required.

Signatures are published outside a component only after joint inference of that component succeeds. Failure discards the component's provisional results. Polymorphic recursion is not supported. The engine's resource-loading order does not restrict inference to independent files.

Initialization cycles are distinct from type cycles. A cycle among const values is rejected because const values are constant expressions. Construction-time member initializers cannot reference self (§8.2). Cross-object construction recursion, such as A initializing a member with `B.new()` while B initializes one with `A.new()`, can overflow the runtime stack.

Changes must not leave stale inferred signatures or executable results in use. Incremental compilation does not provide hot reload, and ordinary runtime calls never trigger inference. Algorithms, coordination and caching strategies are described in the non-normative [design notes](design-notes.en.md#inference-and-incremental-compilation).

## 12. Engine interop: the Variant mapping

Runtime type guarantees depend on the storage that holds a value:

- **Language-exclusive storage:** locals, values captured by closures, and decoded enum values with their field slots. A value satisfies its type when it enters and is not checked again.
- **Language-managed storage that external code can write:** members of the language's classes. When the engine or another script writes through an entry such as `set` or scene injection, the entry check for the declared type applies (§12.6).
- **Engine-managed storage:** Array and Dictionary elements, properties and method results of engine classes, and members of other scripts. The engine and other scripts can write these locations while bypassing static types, so every read used as a trusted type checks the current value against the target type (§12.2, §12.5).

A successful check only establishes that the value was valid at that time; it does not validate the location permanently. Object references are checked for liveness on access, whatever storage holds them; when the static type is a language class, the script identity is checked as well (§6.5).

### 12.1 Values, representations and ABI

Godot Variant is the engine's tagged value container. The language's `Variant` is a non-NIL Variant value, and `Variant?` also includes NIL (§2.1); the binding layer adapts them automatically to engine Variant slots. `Variant` is an interoperation type and does not retain the static types of language values.

A language value converted implicitly to `Variant` uses its type's engine encoding:

| Language type | Encoding when converted to Variant |
|---|---|
| i8…i64, u8…u32 | INT, preserving the numeric value |
| u64 | INT, bits reinterpreted (§12.3); values ≥2⁶³ appear negative |
| f32, f64 | FLOAT, preserving the numeric value |
| enum | The encoding in §12.4 |
| `Option[T]` | Converts only to `Variant?`: None is NIL, and Some is encoded under §12.4 |
| `weak[T]` | A WeakRef object (§12.3) |
| Other types | The representation in §12.3 |

Language values of different types may have the same encoding after conversion to `Variant`, for example the maximum u64 value and the i64 value −1, or an enum encoding and an ordinary Array of the same form. They compare equal as Variants on the engine side and are the same Dictionary key.

`Variant` does not convert implicitly to other types. An explicit conversion to a type T performs the same entry check as an engine write to a location declared as T (§12.5, §12.6); numeric targets also have the `T(v)` numeric conversion of §7.3. The same `Variant` value may pass the checks of several target types; for example, an Array can be read as an Array and may also decode as some enum. The result is determined by the target type. Explicit conversion is written `v as T`, with result `Option[T]`; a conversion that must succeed is written `(v as T)!` (§2.2).

GDScript, the editor, Dictionary keys and resource files can observe external encodings. The rules below specify those encodings by type; they are not selected anew for each value. A later change to an established external encoding is a breaking change.

### 12.2 Containers

- **The language's containers are the engine's containers.** `Array[T]` and `Dictionary[K, V]` are the engine's Array and Dictionary, shared with the engine rather than copied. When the engine can express the element type, the engine's typed container is used.
- The type guarantee has two parts. Writes from the language are checked at compile time, and **every read is checked at runtime**. Reading a value that does not match T is a program error, with a diagnostic such as "element 3 is null, but the element type is Enemy (possibly written by external code)". An implementation may skip the read check when it can prove that only the language ever wrote to the container. That is an optimization and does not change the semantics.
- Rationale: the engine side can write anything into a container. The engine's typed Array allows null for Object elements, and an `Array[i32]` can only be an `Array[int]` on the engine side. If the language used its own containers and copied them at the boundary, it would not see changes the engine makes, which breaks the guarantee that an Array is a shared reference.
- `Array[T].resize(n)` is available only when T has a default under §8.4, and creates that default independently for each new element. Without such a default, `resize(n, fill)` is required. The explicit fill follows the existing value/reference sharing rules and is not deep-copied automatically. The language does not accept the null Object slots left by the engine's raw `Array::resize` as non-null T values (Appendix A).
- Container `==` uses the engine's comparison: elements are compared one by one, their Variant types must match, and float elements follow IEEE 754, whereas NaN components in core value-type elements such as Vector2 and in float packed arrays are treated as equal (Appendix A). Container comparison can therefore differ from the element type's own `==`: two Vector2 values with NaN components are unequal when compared directly but equal as Array elements.
- Dictionary key equivalence differs from `==`: NaN keys are the same key, INT and FLOAT keys are different, and String and StringName keys are the same (Appendix A). In a typed `Dictionary[K, V]` all keys share one encoding type; with `Variant` keys, 1 and 1.0 are different keys even though `1 == 1.0` is true.

### 12.3 Type mapping table

This table specifies external representations for declared target types. The bit transport in the u64 row also applies when a u64 is converted to `Variant` (§12.1). The read checks validate an external representation against its target type; they are distinct from the `T(v)` numeric conversion of a `Variant` value (§7.3).

| Language type | Representation in Variant | Check when read back |
|---|---|---|
| i8…i64, u8…u32 | int | Value is in range |
| u64 | int, bits reinterpreted | None (values ≥2⁶³ appear negative, as with the engine's ObjectID) |
| f32, f64 | float | f32 is rounded; this is a precision loss |
| Core value types (Vector2, String, etc.) | The type of the same name | Type matches |
| Non-null Object class T | Object | Non-null and the class matches |
| `weak[T]` | A WeakRef object; the empty weak is a WeakRef that refers to no object | Must be a WeakRef; a live target must be a T; the target class is checked again on `get()` |
| enum (including Option) | See §12.4 | Fully decoded; see §12.5 |
| `Variant` | Any non-NIL value; language values are encoded under §12.1 | Reject NIL |
| `Variant?` | NIL for None; v for Some(v) | NIL yields None; otherwise Some |
| Function types | Callable | Signature is verified for Callables the language created; others are checked on every call |
| `Array[T]`, `Dictionary[K, V]` | Array, Dictionary | Each element is checked when read |

### 12.4 Enum mapping

This section specifies the engine-facing encoding of enum types. Each enum type has exactly one encoding, determined by its type rather than by the particular value or the enclosing context; conversion to `Variant` uses the same encoding.

1. **Entirely payload-free enums:** when every variant is payload-free, the encoding is INT; numbering and alias rules are specified in §6.1. This includes an enum with only one variant: One in `enum Only: One` encodes as 0, not NIL. Decoding into a closed enum must check named members; open enums (§6.1) are also encoded as INT, and decoding accepts any integer.
2. **Option[T]:** None is encoded as NIL. When T is not an Option type, `Some(x)` is encoded as x's encoding. When T is an Option type, `Some(x)` is encoded as the read-only Array `[&"Option.Some", encoding of x]`, which keeps the levels of None distinct.
3. **Other enums:** when at least one variant has a payload, every variant is encoded as a read-only Array `[&"Enum.Variant", field1, field2, …]`, and a payload-free variant as `[&"Enum.Variant"]`. The leading StringName records the enum name and the variant name, without type arguments: Ok of both `Result[i32, String]` and `Result[f64, String]` uses `&"Result.Ok"`, and the target static type supplies the type arguments. Each field uses its type's encoding recursively.

Among the language's types, only Option types have encodings that include NIL, so rule 2 only needs to test whether T is an Option type. No layout optimization by payload position is applied.

```
enum Mode: Easy, Hard = 4                 # Easy→0  Hard→4
enum Cmd: Stop, Move(Vector3), Hit(Node, i32)
# Stop→[&"Cmd.Stop"]  Move→[&"Cmd.Move", v]  Hit→[&"Cmd.Hit", node, 5]
enum Hp: Dead, Alive(i32)                 # Dead→[&"Hp.Dead"]  Alive→[&"Hp.Alive", 5]
```

An Option has `None` and `Some` values, not a language-level null value. Raw engine null is handled by the binding and read checks (§12.5–§12.6).

| Type | `None` | `Some(…)` |
|---|---|---|
| `i32?` | NIL | `Some(5)` → `5` |
| `Enemy?` | NIL | the object itself |
| `Cmd?` (Cmd above) | NIL | `Some(Cmd.Stop)` → `[&"Cmd.Stop"]`; `Some(Cmd.Move(v))` → `[&"Cmd.Move", v]` |
| `Variant?` | NIL | `Some(v)` → v |
| `i32??` | NIL | `Some(None)` → `[&"Option.Some", NIL]`; `Some(Some(5))` → `[&"Option.Some", 5]` |
| `Variant??` | NIL | `Some(None)` → `[&"Option.Some", NIL]`; `Some(Some(v))` → `[&"Option.Some", v]` |

NIL in these examples denotes an engine encoding, not a language value. For example, the `i32???` value `Some(Some(None))` is encoded as `[&"Option.Some", [&"Option.Some", NIL]]`. Distinct levels of `None` remain distinct and must not be collapsed.

**Encodings and identity:**

Encoding Arrays generated by the language are read-only. Objects compare by identity, both with `==` and as Dictionary keys; Arrays compare and hash by content and can be serialized directly (Appendix A). Read-only encodings may be reused when value semantics are preserved; this does not prescribe the encoding as the internal enum value representation (§12.5). No bit packing is used: a Variant has a single type tag, and copying it preserves only the concrete type's value, not spare bytes in its data area (Appendix A).

An encoding carries no proof of origin. An ordinary Array of the same form decodes as the target enum exactly like an encoding the language generated; without a target type, an encoding Array is just an Array (§12.5). After the engine duplicates or serializes an encoding, the result may no longer be read-only (Appendix A), so decoding does not check read-only status.

Reordering members of an entirely payload-free enum that relies on automatic numbering may change saved meanings; name-tagged encodings change if a variant is renamed. Adding a payload-bearing variant to an entirely payload-free enum changes the encoding of all its variants from INT to name-tagged Arrays, affecting existing saves and every engine-side use of the enum as an integer.

**Depth:** encoding that would exceed the engine's recursion limit (`MAX_RECURSION`, currently 100; Appendix A) is a program error. Decoding enforces the same limit. This guarantees that the language's encoder does not construct an over-depth encoding. It does not guarantee that external code cannot later deepen a shared Array or Dictionary. Normal checks on subsequent reads and object accesses still apply (§12.2, §12.5); the construction guarantee does not validate future external contents.

### 12.5 Reading back and decoding

An external mutable location is checked whenever its contents enter the language as a value of a trusted type. A successful check of a location does not validate that address permanently. Re-reading an engine-managed property or container element requires checking its current value (§12).

For a `Variant` read, NIL is a program error. For a `Variant?` read, NIL is `None`, and a non-NIL value v is `Some(v)`. Other target types are checked as follows:

- Entirely payload-free enums: the value must be an INT; for a closed target enum it must correspond to a named member, while an open enum accepts any INT.
- `Option[T]`: NIL yields None. When T is not an Option type, every other value is decoded as T and yields Some. When T is an Option type, the value must be an Array of length 2 beginning with `&"Option.Some"`, and its second element is decoded as T.
- Other enums: the value must be an Array; its leading StringName must name a variant of the target enum, its length must be one plus that variant's field count, and each field is decoded as its type. Nested enums are decoded recursively. Read-only status is not checked.
- Other types: the checks in §12.3 apply.

An enum and its encoding can be converted explicitly: encoding copies out the representation, and constructing an enum from a `Variant` or an Array performs the decoding above against the target enum type. The target type supplies the nominal type and the required generic arguments.

A tag identifies the type of the encoding, not its construction origin. An ordinary Array of the same valid form may construct an enum through explicit conversion; no Array-identity registry or unforgeable proof of origin is required. Without a target type, an Array is not treated as an enum merely because its shape matches. The concrete APIs for explicit conversion between enums and encodings, and the persistence-version compatibility protocol, are [Open]. Decoding from a `Variant` into a target enum can also be written `v as E` (§2.2).

Successful validation constructs independent enum variant and field slots according to the target type; mutable encoding slots must not serve directly as trusted enum storage. Later changes to the input Array's tag or field slots do not change the constructed enum's variant or field slots. The user's input Array need not be frozen. Objects and containers in fields retain their existing reference-sharing semantics; no deep copy of the entire object graph is required. Nested enum values are likewise constructed under their value semantics, with their reference payloads still shared.

In particular, copying a Variant isolates the value slot, not the contents of an Array or Dictionary it refers to. A checked `Variant` copy therefore does not certify shared container elements; each read follows §12.2. An encoding Array may be modified externally, so every decode checks it again; copying its Variant slot alone does not make it trusted. Non-null Object values still require the liveness checks in §6.5.

The language emits read-only encodings and may reuse them when that preserves value semantics. Receiving an external encoding does not establish that its shared contents will remain unchanged.

### 12.6 Binding contracts and entry conversion

An engine API Variant maps to `Variant` only when a reliable binding contract excludes NIL. Otherwise it maps to `Variant?`. This applies to arguments, return values and properties. The binding layer performs the encoding and checking automatically; ordinary calls do not require source-level conversion to a raw Variant type.

Instances of the language's classes are Godot Objects. GDScript and engine code can therefore bypass static types, for example with `node.call("take_damage", "hello")`, `node.set("hp", 1 << 40)`, scene property values, AnimationPlayer, Tween or clearing an exported reference in the Inspector.

Every incoming Variant is decoded and checked against the declared target type (§12.3, §12.5). This includes callp arguments, `set`, signal callbacks, scene deserialization and Callable calls. A wrong type, an out-of-range integer or absence in a non-null position is a program error. The exception is writing NIL to a member that is still uninitialized: the member remains uninitialized (§8.1). These are checks on external representations, not the integer-to-integer conversions of language values in §7.3; they do not silently truncate. An external Variant INT of 300 read directly as u8 is an error. An explicit `u8(v)` applied to a `Variant` holding 300 yields 44 under §7.3. A direct external read must not be routed through an implicit `Variant` numeric conversion to bypass the target type's range or type checks.

When external code writes NIL to an engine-managed location (§12): if the location is exposed as `Variant?`, the next read yields `None`; if it is exposed as `Variant`, the next read fails the check. The same rules apply when external code modifies an aliased container. Members of the language's classes are checked on write, and a member of type `Variant` rejects a NIL write.

### 12.7 Default values and editor support

Default-value availability and construction follow §8.4. Container resizing follows §12.2.

Editor range hints and debugger display are discussed in the [project plan](project-plan.en.md#tools-and-distribution).

## 13. Modules and engine facilities

The language uses Godot's conventions for script files, resource paths and class registration. It introduces no package manager or elaborate visibility system. Exported properties, signals, resources, scenes and the editor use the existing engine mechanisms. Compatibility checks and the scope of editor support are recorded in the [project plan](project-plan.en.md).

## 14. First-version scope and open questions

The first version includes `Variant` and `Variant?` for interoperation, and external read checks (§2.1, §12). Incomplete constant propagation does not relax the constant-error rule (§7.3).

### 14.1 Known limitations

- No dyn traits. Traits contain only method signatures and serve only as generic bounds (§4). Code that treats objects of different types alike, such as iterating `get_overlapping_bodies()` and calling `take_damage` on each, must obtain the required concrete type with `as` (§2.2) or use a type-erased `call()`.
- No top type within the language. `Variant` is only for interoperation and does not retain the static types of language values (§2.1).
- No recursive enums (§6.1); users cannot declare open enums (§6.1).
- No type narrowing based on None checks, and no binding declaration with an else block (§6.3).
- Return branches of different classes are not merged into a common ancestor (R5); no polymorphic recursion (§11).
- `weak[T]` cannot be used with `@export` and has no `==` (§5.2).
- Static initialization cannot suspend at await (§8.5).
- The result of awaiting a signal with several parameters cannot be used (§9).
- User-defined iterator protocols are not supported (§2.4).
- Setters and getters cannot be coroutines (§8.6).
- No cross-language coroutines: other languages cannot await the results of the language's coroutines, and the language cannot await other languages' coroutines or async values (§9).
- No type suffixes for integer or floating-point literals (§7.1).
- Generic functions are not exposed to the engine (R2).
- No way to turn off bounds checking, and no unsafe mode (§7.6).
- No hot reload and no migration of live state.

### 14.2 Open language questions

- Remaining enum syntax; concrete syntax for traits, generics, remaining match-pattern forms and other conversion forms; and the operator precedence of `as`/`is` (§1.3, §2.2, §6.3, §7.3).
- Conditions for potential-integer-overflow warnings (§7.4).
- Scene-property injection, tree-entry ordering and their interaction with inheritance (§8.3).
- The await lifetime boundaries listed in §9.
- APIs for explicit conversion between enums and their encodings, and the persistence-version compatibility protocol (§12.5).

Deferred language proposals are recorded in the non-normative [design notes](design-notes.en.md#deferred-language-proposals). Engine-version selection, tools and distribution are covered by the [project plan](project-plan.en.md).

## 15. Conformance checklist (non-normative)

This checklist gives examples for checking the rules in the preceding sections; it adds no language requirements.

| Area | Checks |
|---|---|
| Inference | Parameter types inferred from outflows; annotation required when only method calls are present; flowing into Variant or Variant? is not evidence; for locals, inflows first and outflows second, with results independent of the order of constraints; flexible literals typed on the spot at a method call; empty-container members require annotation; branches returning different classes are not merged into a common ancestor |
| Generics | Explicit generics instantiate; generic methods are invisible to `has_method`; instantiating with i32 gives 32-bit semantics |
| SCC | Mutual recursion resolves within a file and across files (including the `make_b().g()` example in §11); generic utility functions outside the cycle are unaffected; conflicts are reported at their real source |
| Enums | Payloads, named returns and exhaustiveness checks; no anonymous sum types are synthesized; enum values cannot be modified in place; a recursive enum is an error that shows the cycle, while recursion through Array, Dictionary or a class compiles |
| Closed enums and integers | User enums and closed imported enums reject unknown integer members; try_from_int returns None for invalid values including constants; E(n) reports invalid runtime values; neither truncates first or accepts non-integers; explicit integer access for payload-free enums follows conversion and constant-range rules; bit operations use integers; payload-free encodings are i64, numbering starts at zero and increments after explicit numbers; out-of-range numbers and increments are errors; explicit numbering in a payload-bearing enum is an error; after a payload-bearing variant is added to an entirely payload-free enum, every variant encodes as a tagged Array; same-number aliases add no values/exhaustive branches and reverse conversion does not promise a unique name; single-variant payload-free enums encode as zero rather than NIL; conversion to Variant yields the INT encoding, and recovery checks named members |
| Open enums | Bitfield-marked engine enums are open automatically; `size_flags_horizontal = SIZE_EXPAND \| SIZE_SHRINK_CENTER` round-trips without losing bits; reading a `key_label` that is a Unicode character reports no error; open-enum values round-trip through parameters, returns and properties in both directions; E(n) accepts any i64 value; a match without a catch-all branch is a compile error; `\|`, `&`, `^` and `~` on one bitfield type produce that type, and operations between different types are errors; the default is 0; a closed engine enum still reports an error for a non-member integer |
| Traits | Compile results when a constraint is satisfied and when it is not, without relying on dyn |
| Nullability | `T?` and `Option[T]` are equivalent; `T??` distinguishes its two levels; the diagnostic when `!` fails; `?.` short-circuits the whole chain; `x != None` does not change x's type, and payloads are obtained with conditional pattern binding; members, properties and container elements are read into a local before obtaining the payload; required `$X` reports a missing node, optional lookup returns Option, and successful lookup does not guarantee later liveness |
| for loops | The loop variable of `range(10)` is i32; `range(a, b)` with i32 and i64 arguments gives i64; `for i: i64 in range(10)` gives i64; `for i in 5` is a compile error with a rewrite suggestion; a step of 0 is an error, a negative step iterates in the right order, and reaching the i32 limit does not overflow; the loop variable over `Array[Enemy]` is Enemy, and a wrongly typed element written externally reports an error when read; annotating the loop variable with a subclass is a compile error; iterating a Dictionary yields keys and ends when the current key is erased; a PackedByteArray yields u8; a String yields one-character Strings; iterating a Variant is a compile error; closures created in the loop each capture that iteration's value |
| Strings | The literal in `Input.is_action_pressed("jump")` becomes a StringName with no runtime conversion; `var a = "jump"` is a StringName if it flows only into StringName parameters; the literal in `get_node("A/B")` becomes a NodePath; an unconstrained literal is a String; a non-literal String passed to a StringName parameter converts implicitly, as does a StringName assigned to a String variable; `&"a" == "a"` is true; ordering comparisons between String and StringName are compile errors; concatenating StringName and String produces a String |
| Equality | `i32 == i64` compiles and `i64 == u64` is a compile error; NaN is not equal to NaN; comparing objects of classes unrelated by inheritance is a compile error; comparing freed objects reports no error; `Some(1) == Some(1)` and `None == None`, and `Option[T]` compared with T wraps it in Some; `Cmd.Move(v) == Cmd.Move(v)`, same-number aliases are equal, and comparing an enum with an integer is a compile error; `[1]` and `[1.0]` in two Variants are unequal, while 1 in a Variant equals 1.0; Vector2 values with NaN components are equal as Array elements and unequal directly; in a Dictionary with Variant keys, 1 and 1.0 are different keys |
| is and as | A subclass conversion gives Some on success and None on failure; conversion between classes unrelated by inheritance is a compile error; `as` on a freed object reports invalid access; after script replacement the new script's class is checked; an INT of 300 gives None for `as u8` while `u8(v)` gives 44; an enum encoding passes both `as Array` and `as Cmd`; `as Array[Enemy]` succeeds for an untyped Array and elements are checked when read; a None source gives None; numeric or String sources are compile errors; accessing subclass members directly after `is` is a compile error with a rewrite suggestion; a failed `(x as T)!` names the actual and expected types; `$X` checks the class when the expected type is a subclass, reports a missing node or class mismatch, and is Node without an expected type |
| Conditional pattern binding | Some, Ok and user enum patterns share rules; evaluate the right-hand side once; successful lexical bindings may be reassigned; mismatch takes else; a bare variable binds the whole value, including None, without unwrapping or truthiness; irrefutable patterns are allowed without an added warning and an unexecuted else is still checked; Some(None) retains the inner None |
| Liveness | A local of a non-null type remains non-null across an await; the referenced Node may no longer be alive and still requires a liveness check; after GDScript or the remote inspector replaces the script of a language object, access through a reference typed with the language class reports invalid access instead of crashing, while access through a Node-typed reference still works; after external code called by a method replaces the script of self, accessing a member of self is an error while the method can still return; replacement by a script that is a subclass of T passes the check; `?.` on a freed object reports invalid access |
| Ownership | A RefCounted object held only by an Object-typed variable stays alive; ownership is unchanged after downcasting; `weak` on a handle type is an error |
| Weak references | get returns Some/None; a local successfully obtained RefCounted value holds a strong reference; an actual Node obtained through weak[Object] remains a handle with liveness checks; static Node types cannot use weak; use conditional pattern binding after one acquisition rather than separating checking and acquisition; GDScript reading a weak member obtains a WeakRef whose `get_ref()` returns the target; writing a WeakRef whose live target has the wrong class, NIL or a non-WeakRef value is an error; `get()` reports an error when the target is alive but not a T; a default weak's `get()` returns None; `@export` of a weak and `==` on weaks are compile errors |
| Reference release and threading | Scope exit, return and program-error exit release strong references whose ownership ends; other owners retain theirs; await suspension retains references held by locals but does not hold self; no reliance on independent-object destruction order; no crash or forced-termination cleanup guarantee; Variant and strong references do not alter engine threading restrictions; no automatic cross-thread release; static cleanup retains code and runtime |
| Sharing and write-back | Changes made through one alias are visible through the others; `line.points.append(p)` writes back to the property; copies follow each type's API contract without guaranteeing a deep copy of the whole graph; user classes may use inherited interfaces and receive no generated copy when none is suitable; enum value copies retain reference-payload sharing; weak back-references and manual cycle breaking work |
| Numbers | `position += velocity * delta` produces no warnings by default; warning classes are independently configurable; integer conversion loss defaults to off, float-to-integer range loss remains on; with `x: u8`, `x + 100` has type u8; `var a: u8 = 300` is a compile error |
| Constants | An unannotated integer literal initializer fixes i32 and a floating-point literal fixes f64; type annotations determine the annotated types; use sites do not change the declaration type; fixed types retain known values for propagation and constant-range checks (§8.1, §7.3) |
| Numeric-conversion syntax | T(x) targets a numeric type; int(x) and float(x) are equivalent to i32(x) and f64(x), int and float are rejected as type annotations, and int(x) rejects i64, u32, u64, enum and Variant sources; direct flexible literals take target context, accepting the largest u64 literal and rejecting u8(300); already typed values follow existing conversions |
| Integer conversions | Narrowing, same-width sign changes, signed and unsigned widening, including negative signed to wider unsigned; all results follow §7.3; in-range constants are accepted, out-of-range constant conversions are errors even with an explicit cast or warnings off; propagation is not stopped by `var`; a runtime range that may lose information is not a constant error |
| Overflow | Overflow in a constant expression is a compile error; the three wrapping methods retain the receiver type; arguments follow call conversions and flexible literals take the receiver type, while out-of-range constant arguments remain errors; operation overflow wraps during constant evaluation and with detection on; turning runtime detection on and off works; all execution modes agree with detection off; an overflow that can only be shown across functions is not a compile error |
| Numeric edge cases | Every row of the tables in §7.5; integer/float common types use full source ranges, with flexible literals typed first; lossy assignment conversions do not determine arithmetic common types; u64 round trip (bit 63 of ObjectID); bounds checking is unaffected by the overflow detection setting; i32 `1 << 31` yields MIN without an error even with detection on; `-8 >> 1` yields −4 and u8 `0x80 >> 1` yields 0x40; shift counts −1 and 32 are errors with detection on and are reduced modulo the width with it off; out-of-range constant counts are compile errors; u8 `b << 4` remains u8; `var m: i64 = 1 << 40` compiles and `var m = 1 << 40` is a compile error; `Time.get_ticks_msec() / 1000.0` is a compile error whose diagnostic suggests `float(…)` |
| Floating-point arithmetic | IEEE 754 rounding and non-finite results; constant overflow to infinity does not invoke integer errors; finite nonzero values divided by zero produce appropriately signed infinity and zero divided by zero produces NaN; no added float-overflow check; float-to-integer retains existing saturation/NaN-to-zero rules |
| Defaults | Each type follows §8.4; Option.None needs no payload default; independent container defaults and each resized default element do not share a mutable instance; explicit fill preserves normal sharing; no implicit default for closed enums, Variant or non-null objects; core empty handles/callables/signals do not guarantee usability; members and static fields without initializers take defaults, locals do not |
| Initialization | A construction-time member initializer that references self is an error, while `@onready` initializers may reference self; an `@onready` initializer that reaches a later-declared member through a method call is caught; `_init` is checked to be callable with no arguments; a child's `_ready` reading a parent's `@onready` member through a signal produces a readable diagnostic; a missing non-null export may be injected later and reports an error when read while uninitialized, without a completion scan; an external NIL write to an initialized non-null member reports an error and keeps the value, including edits from the remote inspector; writing NIL to an uninitialized member leaves it uninitialized; duplicating a node or resource with an unassigned non-null export reports no error and leaves the copy's member uninitialized; the debugger reads an uninitialized member as null without an error; clearing a `T?` member yields None |
| Declarations and definite assignment | Member `var hp: i32` is 0; `var items: Array[Enemy]` is created anew for each instance; a member or static field without a default reports an error naming the member when read before assignment, including from a method called by `_init`, and is usable after assignment; once initialized, a member cannot be returned to the uninitialized state by the language's code or an external write; `@onready` and const without initializers are errors; reading `@onready var label: Label? = …` before its initializer runs is an error rather than None; a local with neither annotation nor initializer is an error; `var x: i32` followed directly by `x += 1` is an error; assignment in both if/else branches is accepted, assignment only in the if branch is an error; assignment in every arm of an exhaustive match is accepted; a closure capturing before assignment is an error |
| Static initialization | Base, declaration and `_static_init` ordering; nested-class dependencies; type-only cycles do not cause execution-cycle errors; internal reads of completed own fields; cross-class reentry, incomplete-field reads and premature construction report dependency chains; explicit initializers do not borrow numeric/Option defaults; valid default initialization without an initializer; reading a static field without a default before assignment is an error; initialization bypasses setters while later assignment is unchanged; await suspension is rejected; failure publishes no usable class and does not automatically retry side effects; one initialization per runtime state; cleanup retains code and runtime dependencies |
| await | Resumption order, reentrancy, cancellation after the instance is destroyed, and frame cleanup, all compared against the engine version selected for evaluation; self is not held while suspended, so after the instance is destroyed a timer firing neither resumes nor reports an error, and references held by locals of the suspended frame are released; with self bound to a local, a RefCounted instance stays alive and resumes normally; after an awaited coroutine is cancelled the awaiting execution stays suspended, and it is cancelled as well when its own instance is destroyed |
| Callable | Known signatures are checked statically; type-erased results never pose as a known type |
| Properties | Reads of a stored property read the field, and engine reads run no user code; writes call the setter, while assignment inside the setter writes the field; declaring a getter on a stored property is a compile error; reads of a computed property call the getter and writes call the setter, also inside the class, and referring to the property inside its own getter or setter is a compile error; assigning a read-only computed property is a compile error, and writing it through an engine `set` reports an error; an initializer or `@export` on a computed property is a compile error, and computed properties do not appear in saves or duplicates; `hp += 1` reads the field and writes back through the setter; `pos.x += 1` writes back through the setter; member and `@onready` initializers do not call the setter; an engine `set` performs the entry check before calling the setter and, on a type mismatch, reports an error without calling it; await in a setter or getter is a compile error whose diagnostic suggests applying the effect once ready |
| Coroutines | A body containing await makes a coroutine; using its value in an expression without await is a compile error; a statement call runs until the first suspension, after which the caller continues and the return value is discarded; await on a plain function call or a Variant is a compile error; an override of `_ready` may be a coroutine, while mismatched coroutine status when overriding a non-void method is a compile error; a void coroutine can be connected to a signal, while converting a non-void coroutine to a non-coroutine function type is a compile error; await in `_init` or a setter is a compile error; adding await to a function makes former callers fail with a diagnostic pointing at that await; an engine call to a coroutine yields NIL at its first suspension |
| Signals | Emitting with mistyped arguments is a compile error; connecting with a different parameter count or non-assignable types is a compile error, while a function parameter that is an ancestor of the signal parameter can be connected; using the await result of a parameterless signal is a compile error; the await result of a one-parameter signal has that parameter's type, and GDScript emitting a wrong type reports an entry-check error at resumption; using the await result of a multi-parameter signal is a compile error; `await area.body_entered` yields `Node2D?`; awaiting a signal with unknown parameters yields `Variant?` with GDScript's shape |
| Variant values | Integers encode as INT and floats as FLOAT without preserving source width; u64 uses bit transport, so a runtime maximum u64 passed through Variant gives that value for u64(v) and −1 for i64(v); the maximum u64 and the i64 value −1 are the same Dictionary key; Variant does not convert implicitly to other types; explicit conversion performs the entry check for the target type, and one value may pass the checks of several target types; Option[T] converts only to Variant?; weak converts to a WeakRef; Object inheritance and ownership are unchanged; assigning one Variant to another adds no container layer; bare NIL is rejected for Variant and becomes None for Variant? |
| Variant mapping | Every table row round-trips; entirely payload-free enums are INT; every variant of other enums, including payload-free variants, is a name-tagged Array; Option None is NIL, and Some uses `[&"Option.Some", …]` when the payload is an Option, so every None layer is distinguished; encodings used as Dictionary keys compare by content; encoding and decoding enforce depth limits; decoding checks variant tag, length and fields but not read-only status, so encodings duplicated by the engine or loaded from saves still decode |
| Enums and encodings | Explicit encoding copies out the representation; reverse conversion decodes against the target type before constructing an internal value; same-form ordinary Arrays may convert without origin registration; the target type supplies the nominal type and generic arguments; later encoding-slot changes do not change enum slots, reference payloads remain shared and input Arrays need not be frozen; without a target type an Array is not treated as an enum |
| External reads and entry conversion | Wrong types, out-of-range external integers and missing non-null values report errors; Array::resize-produced nulls are checked on read; an external NIL write becomes None or an error according to the target; copied Variant slots do not deep-copy Array/Dictionary contents; later external changes are checked on read, not covered by the encoder's construction guarantee; decoded enum value fields remain independent of their encoding Array; members of the language's classes are checked at entry, and engine-managed storage on every read (§12); an external NIL write to a language member of type Variant is reported at the write |
| Incremental compilation | After a change to a function body, a signature or an SCC it depends on, no stale results are reused |
| Engine integration | End-to-end: attaching scripts, callbacks, properties, saving and loading scenes |
| Result propagation | Ok yields its value; Err returns from the current function or closure; the returning boundary requires Result with an identical error type, while success types may differ; Option operands and mismatched error types are rejected; a closure does not return from the enclosing function (§10) |
| Diagnostics | Correct file and location, both types and the line that determined the type, a runtime call stack; no reliance on hot reload |

## 16. Feedback

Feedback is welcome through [GitHub issues](https://github.com/YYF233333/Petrichor/issues). Include the relevant section, a minimal example, the problem it demonstrates and a proposed change if available. A suggested format is **problem → minimal counterexample → section → severity → minimal revision → test**.

Topics for feedback include:

1. Completeness of the mapping table in §12.3, including StringName, NodePath, RID and Signal.
2. The annotations required by R3 and R4 in real projects.
3. Whether the minimum compile-time overflow diagnostics in §7.4 are precise enough to test.
4. Entry conversion (§12.6) and diagnostics for uninitialized export reads (§8.1).
5. Engine compatibility of initialization and await behavior (§8.2, §9).

The development sequence and evaluation work are recorded in the non-normative [project plan](project-plan.en.md).

## Appendix A: Engine facts

Checked against Godot master (4.8-beta, commit `469659e`). Paths are relative to the root of the Godot repository.

| Fact | Source |
|---|---|
| The parameter of `_process` and `_physics_process` is a double | `scene/main/node.h:433-434` |
| `real_t` is float in standard builds | `core/math/math_defs.h:146` |
| `Time.get_ticks_msec()` returns uint64_t | `core/os/time.h:77` |
| Bit 63 of an ObjectID marks RefCounted objects | `core/object/object_id.h:45` |
| RefCounted derives directly from Object | `core/object/ref_counted.h:36` |
| `Array::resize` does not initialize new Object elements | `core/variant/array.cpp:313-325` |
| The engine's typed Array accepts null for Object elements | `core/variant/container_type_validate.h:60` |
| Packed arrays are passed by reference between scripts, but built-in properties and methods return copies | `doc/classes/PackedInt32Array.xml:9` |
| GDScript runs all member initializers, from base class to derived class, before calling `_init` | `modules/gdscript/gdscript.cpp:144, 176, 194` |
| GDScript runs `@onready` initializers, from base class to derived class, when `_ready` is called and before the user's `_ready` body | `modules/gdscript/gdscript.cpp:1956-1972` |
| Properties of native engine classes are defined by a setter and getter method pair | `core/object/class_db.h:457`; `scene/2d/node_2d.cpp:504` |
| GDScript calls getters and setters for property reads and writes inside the class, except inside that property's own getter or setter, where the field is accessed directly; member and `@onready` initializers write fields directly | `modules/gdscript/gdscript_compiler.cpp:304-311, 1292-1369, 2419-2457` |
| A GDScript instance's `set` checks and converts the type before calling the setter, and `get` calls the getter | `modules/gdscript/gdscript.cpp:1555-1579, 1632-1662` |
| A GDScript function whose body contains await is a coroutine | `modules/gdscript/gdscript_parser.cpp:3223-3225` |
| GDScript reports an error when a coroutine call is used in an expression without await; a statement call only raises the `MISSING_AWAIT` warning, which is off by default; await on an expression that is neither a coroutine nor a signal only raises the `REDUNDANT_AWAIT` warning | `modules/gdscript/gdscript_analyzer.cpp:3136-3140, 3820-3827`; `modules/gdscript/gdscript_warning.h:135-136` |
| The GDScript compiler uses untyped temporaries for awaited call results because an override in a subclass may be a coroutine | `modules/gdscript/gdscript_compiler.cpp:610-613` |
| When emitting a signal, the engine only looks up the signal and calls the connected Callables in turn, without checking arguments against the signal declaration | `core/object/object.cpp:1256-1331` |
| In GDScript, the result of awaiting a signal is null with no arguments, the argument with one, and an Array with several; the analyzer types it as Variant | `modules/gdscript/gdscript_function.cpp:237-255`; `modules/gdscript/gdscript_analyzer.cpp:3125-3128` |
| When an await resumes, GDScript also performs a defensive check that the script and the instance still exist | `modules/gdscript/gdscript_function.cpp:274-287` |
| A suspended GDScript function state stores only a raw pointer to its instance, and saving the suspended frame skips the fixed stack slot that holds self, so no strong reference to self is held | `modules/gdscript/gdscript_function.h:325-328, 463-477`; `modules/gdscript/gdscript_vm.cpp:2608-2611` |
| Destroying an instance clears its suspended function states, disconnecting pending signal connections and releasing the suspended frames, so they never resume | `modules/gdscript/gdscript.cpp:2086-2092`; `modules/gdscript/gdscript_function.cpp:303-322, 335-350` |
| MethodBind provides metadata for each argument | `core/object/method_bind.h:102` |
| A Variant is a type tag plus a 16-byte data area (larger in double-precision builds); Transform2D, AABB, Basis, Transform3D and Projection are stored on the heap | `core/variant/variant.h:168, 259-266` |
| Copying a Variant copies only the value of its concrete type | `core/variant/variant.cpp:1187` onward |
| An Array is an ArrayPrivate plus an element buffer; making it read-only allocates an additional Variant | `core/variant/array.cpp:45-49, 958-962` |
| Creating an Object registers it in ObjectDB while holding a global spin lock | `core/object/object.cpp:2370, 2524-2525` |
| Recursive comparison, hashing and text serialization of Arrays and Dictionaries stop at a depth of `MAX_RECURSION = 100` | `core/typedefs.h:260` |
| Beyond that depth, Array equality prints an error and returns true, and hashing returns 0 | `core/variant/array.cpp:148-151, 194-197` |
| Duplicating a Node reads each STORAGE property from the original and writes it to the copy; with `DUPLICATE_INTERNAL_STATE`, other members are copied too | `scene/main/node.cpp:3109-3157`; `scene/main/node.h:110` |
| Resource `copy_from` and duplication likewise read and then write STORAGE properties | `core/io/resource.cpp:234, 247-255, 376, 408-425` |
| The runtime remote inspector reads every script member and property; editing a property calls `Object::set` without checking the result | `scene/debugger/scene_debugger_object.cpp:85, 152`; `scene/debugger/scene_debugger.cpp:848-872` |
| Duplicating an Array and text or binary deserialization all produce new writable Arrays without read-only status; scripts can call `make_read_only`, and read-only status cannot be undone | `core/variant/array.cpp:552-581, 958-962`; `core/variant/variant_parser.cpp:1545-1577`; `core/io/marshalls.cpp:841-882`; `core/variant/variant_call.cpp:2689` |
| Core default construction uses each type's constructor | `core/variant/variant_construct.h:730-742`; `core/variant/variant_internal.h:239-267, 296-335, 848-934, 945-967` |
| Empty String, StringName and NodePath defaults | `core/string/ustring.h:281, 322-323, 678`; `core/string/string_name.h:65, 138-143, 183`; `core/string/node_path.h:54, 105`; `core/string/node_path.cpp:351-353` |
| Vector2, Vector2i, Vector3, Vector3i, Vector4 and Vector4i default to zero components | `core/math/vector2.h:54-58, 207`; `core/math/vector2i.h:54-58, 158`; `core/math/vector3.h:63-65, 222-223`; `core/math/vector3i.h:56-58, 149`; `core/math/vector4.h:51-54, 161`; `core/math/vector4i.h:50-53, 146` |
| Rect2, Rect2i and AABB default to zero position and size | `core/math/rect2.h:42-43, 401`; `core/math/rect2i.h:41-42, 238`; `core/math/vector2.h:350-351`; `core/math/vector2i.h:255-256`; `core/math/aabb.h:45-46, 147` |
| Plane defaults to zero normal and d = 0 | `core/math/plane.h:42-43, 92` |
| Quaternion defaults to (0, 0, 0, 1) | `core/math/quaternion.h:38-41, 125` |
| Color defaults to (0, 0, 0, 1) | `core/math/color.h:39-42, 256` |
| Basis, Transform2D, Transform3D and Projection have identity defaults, with zero origins for the transforms | `core/math/basis.h:41-45, 247`; `core/math/transform_2d.h:58-62, 152`; `core/math/transform_3d.h:43-44, 131`; `core/math/projection.h:55-60, 160` |
| The default RID has ID 0 and is not a valid resource handle | `core/templates/rid.h:40, 61-62` |
| The default Callable is empty and invalid; calling it reports an error | `core/variant/callable.h:49-52, 89-90, 135`; `core/variant/callable.cpp:44-49, 151-156` |
| The default Signal is empty and unbound; emit/connect require an existing object | `core/variant/callable.h:179-184, 205`; `core/object/object_id.h:42, 60`; `core/variant/callable.cpp:532-545` |
| Array and Dictionary default constructors create new empty backing containers | `core/variant/array.cpp:45-51, 983-986`; `core/variant/dictionary.cpp:43-50, 781-784`; `core/templates/hash_map.h:72-78, 681-683` |
| Packed array types use Vector; default Vector and CowData are empty | `core/variant/variant.h:82-92`; `core/templates/vector.h:71, 318`; `core/templates/cowdata.h:88, 185, 233` |
| An Array is iterated by index, with its length rechecked at each step | `core/variant/variant_setget.cpp:1417-1423, 1654-1662, 1833-1842` |
| Iterating a Dictionary yields keys, finding each next key from the current one; iteration ends when the current key has been erased | `core/variant/variant_setget.cpp:1406-1415, 1643-1652, 1829-1830`; `core/variant/dictionary.cpp:581-599` |
| Iterating a String yields one-character substrings | `core/variant/variant_setget.cpp:1398-1405, 1633-1642, 1825-1827` |
| GDScript allows String to convert implicitly to StringName and NodePath, and those to convert back to String | `core/variant/variant.cpp:560-575, 607-612, 763-778`; `modules/gdscript/gdscript_analyzer.cpp:6419-6423` |
| Constructing a StringName from a String hashes it and looks it up in a global table under a mutex | `core/string/string_name.cpp:280-302` |
| Variant defines `==` and `!=` between String and StringName, and concatenation yields a String | `core/variant/variant_op.cpp:196-201, 222, 493, 615` |
| Variant `==` compares INT and FLOAT numerically | `core/variant/variant_op.cpp:490-492` |
| Array and Dictionary `==` call `hash_compare` on each element with semantic comparison off; it requires matching Variant types and compares floats under IEEE 754, while core value types such as Vector2 compare with `is_same`, treating NaN as equal, as do float packed arrays | `core/variant/array.cpp:135-154`; `core/variant/dictionary.cpp:278-295`; `core/variant/variant.cpp:3175-3194, 3213-3224, 3365, 3369`; `core/math/vector2.cpp:207-209`; `core/math/math_funcs.h:584-589` |
| Dictionary keys use `hash_compare` with semantic comparison on, and treat String and StringName as the same | `core/variant/dictionary.cpp:46`; `core/variant/variant.h:837`; `core/variant/variant.cpp:3437-3448` |
| GDScript's object `as` yields null on a class mismatch and reports an error for a freed object in debug builds; `as` with a built-in type is a conversion through `Variant::construct` that reports an error on failure | `modules/gdscript/gdscript_vm.cpp:1615-1642, 1644-1673, 1675-1722` |
| `Object::set_script` deletes the old script instance and then creates a new one; the object and its ObjectID are unchanged; `set_script()` has no veto hook and is exposed to scripts | `core/object/object.cpp:1049-1077, 1921` |
| `Object::set` offers a property to `ScriptInstance::set` before handling the `script` property, so a script instance can intercept writes through `set("script", …)` but not direct `set_script()` calls | `core/object/object.cpp:204-211, 227-228` |
| The runtime remote inspector modifies an object's `script` property through `Object::set` | `scene/debugger/scene_debugger.cpp:862-863, 871` |
| WeakRef is a RefCounted that stores only an ObjectID and binds only `get_ref`; the methods that set its target are not exposed to scripts; `weakref()` creates a WeakRef, and `get_ref()` returns null when the argument was null or no target is set | `core/core_bind.h:700-712`; `core/core_bind.cpp:2364-2391`; `core/variant/variant_utility.cpp:816-844` |
| Property and argument information marks bitfield types with `PROPERTY_USAGE_CLASS_IS_BITFIELD` | `core/object/property_info.h:102`; `core/variant/type_info.h:258, 268` |
| Mesh.ArrayFormat is a bitfield, including shift amounts such as `ARRAY_FORMAT_CUSTOM0_SHIFT` | `scene/resources/mesh.cpp:872-904` |
| `key_label` can be the uppercase form of any Unicode character whose code point is at least 0x20 and not 0x7F; the Windows platform layer sets `key_label` this way | `core/os/keyboard.cpp:496-501`; `platform/windows/display_server_windows.cpp:7145, 7165`; `doc/classes/InputEventKey.xml:66-67` |
| `get_*_with_modifiers` returns Key values with modifier bits added; C++ defines `\|` and `&` between Key and Key or KeyModifierMask | `core/input/input_event.cpp:399-408`; `core/os/keyboard.h:293-330` |
| Enum metadata stores names, int64 values and a bitfield flag, not the original declared width | `core/object/gdtype.h:48-52`; `core/object/class_db.h:488-495`; Godot `469659e0617345149cd1eaec2e643d61d130aa6a` |

## Appendix B: Alternatives considered (non-normative)

This appendix records design rationale. The rules in the referenced sections define the language.

| Alternative | Why it was not adopted |
|---|---|
| Borrowing by default, explicit move, or moving values into containers automatically | Too much annotation for scripting; shared references are used instead, and external types follow the engine's contracts |
| Tracing GC | Replaced by reference counting, weak references and manual cycle breaking |
| Encoding `weak[T]` as the target's ObjectID integer | GDScript would see only an integer, with no sign that it is a weak reference and no `get_ref`; the engine's WeakRef is used instead (§5.2, §12.3) |
| Plain references keeping Nodes alive automatically | Node lifetimes belong to the engine; handles do not extend them |
| Forbidding script replacement on language objects | Direct `set_script()` calls have no veto hook; a script instance can intercept only the `Object::set` path, so replacement cannot be forbidden entirely (§6.5) |
| Intercepting script replacement through `set("script", …)` | Direct `set_script()` calls cannot be intercepted, so the script-identity check remains necessary; intercepting would only remove the runtime inspector's ability to replace scripts while debugging (§6.5) |
| Checking liveness only, not script identity | After a script is replaced, accesses would read a deleted instance, which is undefined behavior (§6.5) |
| HM-style inference with implicit generalization of ordinary functions | Conflicts with the engine calling functions by name (R2); semantics are defined first (R1–R7) and generics are explicit |
| Inferring a function's type from its callers | The type would change whenever a call is added, and dynamic callers are invisible (R1) |
| Rejecting a local whose constraints disagree (as Rust does) | In the surveyed corpus, the conflicts did not identify a bug; R4 uses inflows first, then outflows (see the survey and its limitations) |
| Typing a local from its initializer alone (as Swift, Kotlin and C# do) | `var r = 0` followed by assigning a double would truncate the result to 0 (R4; see the survey) |
| Implicit defaults for locals declared without initializers (as GDScript does) | A local's control flow is visible to the compiler; definite assignment detects missed assignments and lets non-null locals be declared before assignment (R4) |
| `@onready` members whose types have defaults reading those defaults before their initializers run (as GDScript does) | Consistent with §8.5, where a field with an explicit initializer cannot be read before the initializer completes; reading a default would hide initialization-order errors (§8.2) |
| Requiring an initializer for every member without a default | Common patterns such as assignment in `_init`, assignment in `_ready` and static singletons would all need `T?`; the uninitialized state of §8.1 is generalized instead |
| An abstract int whose width is decided further along the call chain | Replaced by a full family of fixed-width types, with i32 when nothing constrains the type |
| f32 as the default float | f64 is the default; select f32 with a type annotation or explicit numeric conversion, not a type suffix |
| int as a type alias for i32 | In GDScript and the engine, int is 64-bit, so reusing the name would silently truncate ported code; int is not a type, and `int(x)` rejects i64, u32, u64, enum and Variant sources (§7.1, §7.3) |
| A single warning class for all lossy implicit conversions | Precision loss, integer conversion loss and float-to-integer range loss are configured separately; integer conversions default to silent truncation (§7.3) |
| any as a top type within the language, retaining the dynamic types of language values in typed dynamic boxes | The engine carrier cannot distinguish a box from an Array of the same form, and duplication and serialization keep neither read-only status nor identity, so `any is E` and `any is Array` cannot give unique answers; the first version keeps only `Variant` for interoperation, with heterogeneous values deferred to dyn trait (§2.1, §12.1) |
| Enum encodings optimized by payload position: NIL or INT for payload-free variants, single-field variants inlined as the field's encoding, and full boxing on conflict | The encoding varies with the combination of variants and needs conflict analysis, and a variant encoded as NIL cannot convert to a non-null Variant; instead, entirely payload-free enums use INT, Option uses NIL, and all others use name-tagged Arrays (§12.4) |
| All imported enums closed, with each position carrying values beyond named members corrected to an integer | Bitfield types and constants are numerous and already marked by the engine; per-position corrections are easy to miss, and a missed position fails at runtime; open enums are marked per type instead (§6.1, §7.6) |
| A typed box for u64 converted to Variant | Variant does not retain language types; like a static u64 slot, u64 is transported bitwise as INT, and `u64(v)` recovers the original value (§12.1) |
| Bare NIL as a value of Variant | Variant is non-null; absence is the None of `Variant?`, encoded as NIL |
| Treating bits shifted out by a left shift as overflow | Hashes and masks rely on discarding shifted-out bits; Rust, Java and C# also check only the shift count (§7.5) |
| Typing shifts by the common type of both operands | u8 `b << 4` would become i32, and a literal left operand would take the count's type; the left operand's type is used instead (§7.5) |
| f64 as the result of mixing u64 or i64 with floats | This needs a lossy common type, contrary to the lossless common-type rule; write `float(x)` explicitly (§7.5) |
| Rejecting all potentially lossy runtime integer conversions | Runtime conversions have defined modular results; only out-of-range constant conversions are mandatory errors (§7.3) |
| Overflow as undefined behavior when detection is off | It would break determinism (lockstep multiplayer, replays); the result is defined to wrap instead |
| Constants computed at runtime and then frozen | Only constant expressions are allowed |
| Untyped const declarations whose type is selected at each use | A const declaration fixes its type from its annotation or initializer; its value remains available for constant propagation (§8.1) |
| Iterating a Dictionary as key-value pairs | Not expressible without tuples; it can be added compatibly once destructuring exists (§2.4) |
| Allowing iteration over a `Variant` | Elements could only be `Variant`, deferring type errors to runtime; converting to an Array first costs one step (§2.4) |
| Requiring explicit conversion between non-literal String, StringName and NodePath values | Building a name and passing it is common in GDScript; these conversions lose no information, so requiring them explicitly would be inconsistent with lossless numeric conversions (§7.7) |
| Fixing string literals as String and relying on runtime conversion | Each use of a constant string would require a global-table lookup; flexible literals are constructed at compile time (§7.7) |
| Comparing containers element by element with the language's `==` for the element type | The same comparison would give different results in GDScript and in the language; containers are engine containers, so they compare under engine rules (§2.3, §12.2) |
| Comparing across numeric types directly as Variant does (such as `i64 == f64`) | This needs a lossy common type, contrary to the lossless common-type rule of §7.5 (§2.3) |
| `as` reporting an error on failure, with a separate nullable `as?` (as in Kotlin and Swift) | This inverts GDScript's meaning of `as`, which yields null on failure, so ported code that checks for null after `as` would report errors; `as` returns an Option instead, and `(x as T)!` is used when it must succeed (§2.2) |
| Implicit downcasts driven by the expected type (like GDScript's `var e: Enemy = node`) | All type errors would be deferred to runtime; only `$X` checks against the expected type (§2.2, §6.5) |
| A type pattern `if var e: Enemy = body` | It has the same form as an annotated declaration, so readers cannot tell an annotation from a test; `if var Some(e) = body as Enemy` is used instead (§2.2) |
| Flow-sensitive narrowing based on None checks | The recommended forms are conditional pattern binding and `?.`, giving one set of pattern rules for obtaining payloads; narrowing can be added compatibly later (§6.3) |
| `T?` and Option as separate concepts | Generic code needs nesting, and two separate APIs for absent values would split the ecosystem; `T?` is `Option[T]` |
| Encoding enums as RefCounted objects | Objects compare by identity, which breaks Dictionary keys; read-only Arrays are used instead |
| Bit-packing small enums into INT or Vector4i | There is nowhere to store which variant a value is, float equality would depend on the representation, and the debugger would show raw numbers |
| Language-owned containers, copied at the boundary | Changes made on the engine side would be invisible; engine containers are used directly, with checks on read |
| Recursive enums with automatic indirection | Indirection introduces allocation; the first version rejects recursive value layouts (§6.1). An explicit-indirection proposal is recorded in the design notes |
| Building a file-level dependency graph before inference | Method dependencies can depend on inferred receiver types; joint inference is defined per declaration (§11), with discovery strategies described in the design notes |
| Full row polymorphism and statically forwarding arbitrary arguments | Parameter packs are listed as future work |
| GDScript-style setget, where a stored field may have a getter | Engine duplication, saving and debugger reads run the getter, proxy and derived properties leave useless storage, and read-only properties cannot be declared; properties are split into stored and computed instead, and the survey found getters needing manual rewriting about 0.12 times per 1,000 lines (§8.6) |
| Allowing setters and getters to be coroutines (such as `await ready` in a setter) | Asynchrony hides behind assignment, and the order in which side effects apply depends on signal connections; rewriting to apply once ready is mechanical and can be relaxed compatibly later (§8.6) |
| Bypassing the setter for property access inside the class (Godot 3 setget) | GDScript 4 goes through the setter, and ported code depends on that (§8.6) |
| Calling the setter during initialization | The setter may read other members not yet initialized, defeating §8.2, and this differs from GDScript (§8.6) |
| Declaring coroutines with an explicit marker (such as `async func`) | Adds annotation burden and differs from GDScript; coroutine status can be inferred from the body (§9) |
| Allowing overrides to change coroutine status freely (as in GDScript) | Callers cannot know statically whether await is needed, and return types become unreliable (§9, Appendix A) |
| `Array[Variant]` as the await result of a multi-parameter signal (as in GDScript) | All type information is lost, inconsistent with static types for callbacks with known signatures (§9); binding by parameter is in the design notes |
| Requiring function parameter types to equal signal parameter types exactly when connecting | Stricter than allowing ancestor parameter types, with no safety benefit (§9) |
| A suspended execution holding a RefCounted self | If the awaited signal never fires, the object leaks indefinitely; this also differs from GDScript (§9) |
| Reporting an error when resuming a suspended execution after its instance is destroyed | GDScript disconnects pending waits and cancels suspended executions when an instance is destroyed; the common pattern of awaiting and then calling `queue_free` would report false errors (§9) |
| Hot reload and live state migration | Not planned |
