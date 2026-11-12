# Petrichor Language Specification

**v0.2 draft · 2026-10-25**

<!-- Generated from language-spec.zh.md, git blob 50457defd19a7d714cce631f282b4fa3be29d8ed. Do not edit by hand; see tools/spec_translation.md. -->

Petrichor is a fully statically typed scripting language for Godot 4, shipped as a GDExtension. It replaces GDScript in project code and coexists with GDScript plugins. This document is the draft of its language specification. Not every rule is implemented yet ([implementation](architecture.md)). The design trade-offs and the corpus evidence are in the [design notes](design-notes.md). The baseline for checking engine behavior is in the [engine facts](engine-facts.md). The previous version of the specification, v0.1, is kept in the git tag `spec-v0.1`. This English edition is generated from the [Chinese edition](language-spec.zh.md). Where the two differ, the Chinese edition prevails. The linked documents are in Chinese only.

## 1. Overview

### 1.1 Problem and goals

- GDScript's typing is gradual. Static information in scenes does not reach scripts: node paths, signal connections, animation tracks and resource properties. C# has static types, but its cross-language bridge and toolchain have many pitfalls.
- This language is fully statically typed. Dynamic values appear only where the engine or a third party imposes them. Static checking extends into scenes. Node paths, scene signal connections, animation tracks, exported properties and scene resources all have types. A rename changes the scenes as well.
- The larger a project and the longer it is maintained, the greater the benefit. This holds whether people write the code or AI generates it. Code that nobody reviews depends even more on static checking.
- A new project uses the language directly. An existing project can migrate as a whole or in batches, and can go back to GDScript. Third-party plugins stay in GDScript.

### 1.2 Relation to GDScript

- The surface syntax follows GDScript: indentation, `func`, `var`, `extends`, `signal`, `@export`, `$X` and so on. New constructs extend it in the same style.
- **The syntax hard rule**: a construct written as in GDScript either behaves as in GDScript or is a compile error. The error gives a fix. The construct is never silently different. The only exception is where GDScript fails at run time or silently gives a wrong result. Examples are a default value, a truncation and hanging forever. There, this language makes it a program error, or does what the code originally intended.
- **The reversibility rule**: what GDScript code means can always be written in this language with only two kinds of forms:
  - forms that GDScript also has, with the same meaning;
  - reversible forms, that is, forms whose removal or replacement by a fixed rule gives GDScript of the same meaning. Examples are `pub`, `async`, `.detach()` and `?` in types.
- The reversibility rule has these exceptions: those of the syntax hard rule, and each **[Known limitation]** marked where it applies.
- There is no promise to compile GDScript source code directly. The migration tool converts in both directions by the reversibility rule.

### 1.3 Delivery

- The language ships as a GDExtension. It installs into the official editor and uses the official export templates. A Web export must enable "Extensions Support" (cross-origin isolation headers are needed only when thread support is enabled).
- Source code is compiled only in the editor and in headless mode. An exported game contains only bytecode, and the export template libraries contain no compiler.

### 1.4 Conventions of this document

- Rules that define legal programs and required behavior are normative. Examples, the [design notes](design-notes.md) and the [engine facts](engine-facts.md) are not.
- The first version means version 1.0. Versions before 1.0 may still change the semantics.
- Status markers: **[Open]** means not yet decided. **[Known limitation]** means the first version does not support it, but its restrictions on legality are still normative. The chapter "First-version scope and open questions" collects the open questions. It also collects the features that can be added compatibly after the first version.
- A **compile error** is reported by the analysis service before the program runs, and makes the program illegal. The only exception is a missing-value error marked on a scene, which does not prevent running in the editor. **Warnings** and **hints** do not affect legality. A **program error** is a runtime error, handled as the chapter on errors says.
- GDScript here means the GDScript of Godot 4. The syntax hard rule and the reversibility rule both take it as their reference.
- The engine behavior relied on here is checked against Godot 4.7.2-stable (commit `ed1daf0`). The sources are listed in the [engine facts](engine-facts.md).
- The extension of source files is `.pet`.

### 1.5 Guarantees

Besides the syntax hard rule, this language's own operations guarantee the following three points everywhere. The behavior of engine APIs does not change. The only exceptions are each **[Known limitation]** marked where it applies.

- **Types can be trusted**: a value not fitting its static type is a program error before use as that type. A non-null `T` is never null, but it may be a freed object.
- **No silent failure**: a failure is a compile error, gives an `Option` or a `Result`, or is a program error. Nothing silently gives a default value or a wrong result, and nothing hangs forever. Except for `assert`, detection and handling are exactly the same in debug and release builds.
- **No crashes**: a legal program does not crash the process or corrupt memory, except through data races on shared objects.

## 2. Lexical structure and surface syntax

### 2.1 Files and classes

- `preload` of any script gives its class. A constant initialized with it names that class where its bare name is used. Like a `class_name`, this name can serve as a type, and gives access to `new()`, static methods and constants. The same holds for `.new()` or another member accessed directly after `preload(...)`. Where a value is needed, it is the script resource itself. Its type is `GDScript` for a GDScript file, and the script resource class for any other script:

  ```gdscript
  const Unit = preload("res://units/unit.gd")

  var squad: Array[Unit] = []

  func spawn(at: Vector2) -> Unit:
      var unit := Unit.new()
      unit.position = at * Unit.TILE_SIZE
      return unit

  func adopt(body: Node2D):
      if body is Unit:
          squad.append(body)
      else:
          body.set_script(Unit)
  ```

- A class header may contain class-level annotations, `class_name`, `extends` and `implements`, all written before the members. `class_name` and `extends` come in either order, as in GDScript. The last two may share the line of `class_name`, or each take a line of its own:

  ```gdscript
  class_name Player extends CharacterBody3D implements Combat.Damageable
  ```

- A file may declare inner classes, structs, enums and interfaces, which all belong to the class of that file. Inner classes are written as in GDScript, and `implements` may be written wherever `extends` may.

### 2.2 Lexical structure

- Indentation, comments, identifiers, literals and operators follow GDScript, with these additions and removals:
  - New contextual keywords: `pub`, `struct`, `interface`, `implements`, `async`, `None`. They are keywords only in their own syntactic positions. After a dot and in other positions, they are ordinary names.
  - A `None` standing alone is always the empty Option. **[Known limitation]** A declaration named `None` that a bare name can reach is a compile error. Examples are a variable, a function and a value of an anonymous enum.
  - A new postfix operator `?` for short-circuit return.
  - `null` is an alias of `None`, with the same semantics as `None`. A hint suggests writing `None` in new code.
  - Single-quoted and double-quoted strings cannot span lines. A string that spans lines must use triple quotes.
  - `##` documentation comments also apply to structs and their fields, and to interfaces and their members. A comment that begins with `###` is an ordinary comment.

### 2.3 Annotations

- **[Open]** `@tool` and `@export_tool_button` make a script run in the editor. They belong to the first version, and their rules are not settled yet. Until they are settled, they are compile errors.
- `@warning_ignore` takes the snake_case names of this language's warnings and hints. Each hint has a name, as each warning does. A name that suppresses no diagnostic gives the hint `unused_ignore`. This includes GDScript warning names left over from migration.
- New class-level annotations: `@parent(T)` and `@owner(T)`, for upward access. New member annotation: `@readonly`, only for interface properties.
- `pub` is written after the annotations and before `static`.

### 2.4 How declarations are written

- **struct**: under `struct Name:`, `var` declares the fields, which may have default values. Construction takes named arguments only, and a field with a default value may be omitted:

  ```gdscript
  struct Hit:
      var damage: int
      var point: Vector3
      var crit: bool = false

  var h = Hit(damage = 3, point = p)
  ```

- **enum**: a variant may carry named payload fields, and is constructed with arguments passed by name. An enum without payloads is the same as in GDScript:

  ```gdscript
  enum Command { IDLE, MOVE(target: Vector3), ATTACK(target: Node3D, power: int) }

  var c = Command.MOVE(target = Vector3.ZERO)
  ```

- **interface**: an indented block that may declare `var`, `signal` and `func` without a body. A method is written without the colon. By default a property is required to be readable and writable. One required only to be readable is marked `@readonly`. All interface members are public:

  ```gdscript
  pub interface Damageable:
      var hp: int
      signal died
      func take_damage(amount: int)
      func is_dead() -> bool
  ```

- **Implementing and extending interfaces**: a class header writes `implements A, B`. An interface that extends interfaces is written `interface Combatant extends Damageable, Healable:`.
- **pub**: a keyword used on members and on structs, enums, interfaces and inner classes. Examples are `@export pub var max_hp: int` and `pub static func make() -> Player`.
- **Placement and naming**: structs, enums, interfaces and inner classes belong to the class of their file. Inside that file, the name is written directly. Other files write `ClassName.Name`, such as `Combat.Hit`.
- **Function types**: they follow the shape of a function declaration, without the function name and the parameter names:

  ```gdscript
  var on_hit: func(int, Node3D)                 # no return value; -> void may also be written
  var score: func(Enemy) -> float
  var load_level: async func(String) -> Level   # a call gives Task[Level]
  var filter: (func(Enemy) -> bool)?            # nullable needs parentheses, or ? belongs to the return type
  ```

### 2.5 Statements

- Statements are written as in GDScript.

### 2.6 GDScript's own constants and functions

- GDScript's own constants and utility functions follow GDScript, such as `PI`, `range()` and `len()`. A declaration with the same name shadows them.
- `Color8`, `convert`, `type_exists`, `inst_to_dict` and `dict_to_inst`, which GDScript has deprecated, are compile errors. The diagnostic gives the replacement that the Godot documentation suggests.
- The `T` of `is_instance_of(v, T)` is one of two kinds:
  - a written class name or a `TYPE_*` constant, which means the same as `v is T`;
  - a value whose static type is `Script`. It is tested by the rules of `is`, but does not narrow the type of `v`.
- **[Known limitation]** An engine class stored in a variable cannot be tested this way.

## 3. Types

### 3.1 Overview

| Category | Types this language adds or changes |
|---|---|
| Objects | non-null `T` and nullable `T?`; `PackedScene[T]` |
| Value types of this language | struct, enum |
| Interfaces | interface |
| Standard types | `T?` (Option), `Result[T, E]`, `Task[T]` and `Settled[T]`, `ConversionError` |
| Functions | function types |

Numbers, strings, core value types, containers, `Callable`, `Signal` and `Variant` follow GDScript's types. The sections below state only the differences.

### 3.2 Numbers

- There are no narrow integer types. Fixed-width arithmetic uses the wrapping arithmetic methods and the bitwise operations on `int`.

### 3.3 Objects and nullability

- An object type `T` is non-null. `T?` is an object or `None`.
- After an object is freed:
  - every read of a `T?` reference does a liveness check, consistent with `is_instance_valid`, and a freed object reads as `None`. So narrowing by `!= None` is also a liveness check;
  - a `T` reference may still hold a freed object. Reading it, comparing by ObjectID with `==` and ordinary argument passing do not count as access. Accessing its members is a program error. For instances of this language and objects it freed, a debug build states the free site in the diagnostic;
  - passing a freed object to an engine parameter declared with an object type is a program error. So is writing it to an engine property of an object type. So is passing it to a method of an `Array[T]` with object elements, such as `append`, `erase` and `has`. Parameters declared as `Variant` are not restricted this way, e.g. `is_instance_valid` returns `false`;
  - for any static type, `is` on a freed object is `false` and `as` is `None`;
  - when a running method's object is freed or loses this language's script, the method may run on until it returns. Accessing a member or `self` after that event is a program error.

### 3.4 Option

- `T?` is Option, whose value is `Some(v)` or `None`. An explicit wrap is written `Some(v)`, such as `Some(None)` in `T??`.
- All operations are methods, named as in Rust. `unwrap` and `expect(message)` are program errors on failure. The others are `unwrap_or`, `unwrap_or_else`, `map`, `and_then`, `or`, `or_else`, `filter`, `ok_or` and `ok_or_else`. There are no `!`, `??` or `?.` operators. The only special syntax is the postfix `?` for short-circuit return. A test for `None` uses `!= None` and narrowing.
- A nested Option keeps its levels, such as `T??`:
  - the literal `None` is always the outermost `None`;
  - `?`, `unwrap`, `match` and narrowing each peel one level at a time, and `.flatten()` merges `T??` into `T?`;
  - the same holds for `Variant?`: `None` means no value, and NIL is a value.

  ```gdscript
  var r = enemies.pop_back()   # enemies: Array[Enemy?], r: Enemy??
  match r:
      None: print("空数组")
      var e: print(e)          # e: Enemy?, may be None
  ```

### 3.5 Result

- `Result[T, E]` is constructed with `Ok(v)` and `Err(e)`. A success value is not wrapped implicitly.
- The methods are named as in Rust: `unwrap`, `expect`, `unwrap_or`, `unwrap_or_else`, `map`, `map_err`, `and_then`, `or_else`, `ok`, `err`. On `Err(e)`, the postfix `?` returns `Err(e)`.
- Discarding a `Result` is a compile error. To ignore it deliberately, write `.ok()`.

### 3.6 struct

- Value semantics: assignment, argument passing and return all make a shallow copy, and object and container fields copy the reference. A struct exists only inside this language. Construction takes named arguments only.
- Across the engine boundary, a struct is encoded by the rules of the engine boundary. Decoding checks each field and each nested container or struct recursively, by the rules of `T.try_from`. Extra keys are ignored. A missing key takes the default value of the field. When the field has no default value, decoding fails.
- A struct may declare `func` and `static func`. In an instance method `self` is read-only, and assigning to a field of `self` is a compile error.
- A name in a method is looked up in the struct itself first. It is then looked up in the non-instance declarations of the outer classes, by the rules of inner classes.
- When a construction omits a field, the default value of that field is evaluated once each time. Names in a default value are looked up by the rules of static methods, and cannot read fields.
- Fields and methods are visible wherever the struct is visible.
- The modification `s.x = 1` acts on the storage location that holds `s`:
  - a chained assignment reads, modifies and writes back along the chain. `player.last_hit.damage = 3` runs the setter of `last_hit`, and `hits[0].damage = 3` updates the array element;
  - an assignment first evaluates the objects and indexes in the location from left to right, except the last index. It then evaluates the value on the right and the last index, as in GDScript. Then it reads, modifies and writes back in one go, without suspending in between. The same holds for engine value types, such as `position.x`, and for compound assignments. A method call that modifies its receiver, such as `points.append(x)`, first evaluates the location of the receiver. It then evaluates the arguments, and then reads, calls and writes back;
  - assigning to a field of a temporary value, such as `get_hit().damage = 3`, is a compile error;
  - loop variables and lambda captures are copies. When a struct loop variable has a field modified and is not read afterwards, it gives the `modified_copy` warning. Modifying a shared value through a container or an object field does not count.
- By default a struct provides a field-by-field `==` and a hash over all its fields. So every struct can be a `Dictionary` key. The hash computes containers by their contents. When all fields have `<`, the struct also provides a lexicographic `<` in declaration order. Object fields have no `<`, so a struct with an object field has none either.
- A struct or an enum with payloads that contains itself without going through a container is a compile error. This holds for direct and indirect containment, and the containers are `Array` and `Dictionary`. The diagnostic lists the fields that form the cycle. Recursion through a container is legal.
- Suppose a value of a struct or an enum with payloads nests through containers more than 100 levels deep. Comparing, hashing, converting to a string, encoding, decoding and deep-copying it are then program errors. So a value that contains itself through a shared container is an error too.

  ```gdscript
  struct Hit:
      var damage: int
      var crit: bool = false

      func total() -> int:
          return damage * 2 if crit else damage

  player.last_hit.damage = 3   # read, modify, write back through the setter of last_hit
  get_hit().damage = 3         # compile error: modifies a temporary value
  for h in hits:
      h.damage = 0             # warning: modified_copy
  ```

### 3.7 enum

- An explicit enum value must be an `int` that the compiler computes. An anonymous `enum { A, B }` declares `int` constants of the class.
- An enum of this language is closed. Its values are validated by the entry check and when read from a container. An engine enum is open: the engine produces values outside the enum, and later versions may add values. So `MouseButton(x)` does not check the range, and a `match` on it must have `_`.
- Converting an `int` to an enum of this language must be written explicitly. Write `State(x)`, a program error when `x` is not an enum value, or `State.try_from(x)`:

  ```gdscript
  enum SpeedUnit { KMH, MPH }
  var unit := SpeedUnit.KMH
  unit = SpeedUnit((unit + 1) % SpeedUnit.size())
  unit = 1                                          # compile error; the quick fix changes it to SpeedUnit(1)
  ```

- When an enum name is used as a value, its methods are typed: `keys() -> Array[String]`, `values() -> Array[State]`, `find_key(v) -> String?`, `get(name) -> State?`. `State[name]` has type `State`, and is a program error when the name does not exist. A constant initialized with an enum name names the same enum.
- An enum without the value 0 has no default value, and its variables must be assigned.
- An enum with payloads:
  - payload fields must be named, such as `MOVE(target: Vector3)`. They can be destructured only through `match`, and writing `c.target` is a compile error;
  - `==`, `<` and the hash follow the rules of structs. They go by variant first, with `<` in declaration order, and then by fields within the same variant;
  - its name cannot be used as a value, as in `Command.keys()`. A constant initialized with it still names the same enum;
  - **[Known limitation]** it cannot declare methods in the first version.

### 3.8 Containers

- **Conversion with untyped containers**: when a container's runtime type is exactly `Array[int]`, `a is Array[int]` is true and narrows, without copying. `assign()`, by contrast, copies the elements. Suppose a value is written into a typed container through an untyped view. When the value is not assignable to the element or key type, the write is a program error. So is looking up such a value through an untyped view, with `has`, `find`, `erase` and so on. So is `assign()` when it meets such an element. A `None` written into an `Array[Node]` is also a program error when it is read.
- **Representation and nesting**:
  - when the engine can represent the elements, the engine's typed container is shared directly;
  - a container whose element type exists only in this language uses a representation of its own. Such types include structs and enums with payloads. `Array(hits)` and `Dictionary(d)` make an untyped copy, encoded by the rules of the engine boundary. The reverse direction uses `Array[Hit].try_from(v)`;
  - a nested container, such as `Array[Array[int]]`, keeps its inner types. They are validated by the entry check and when an inner container is read.
- **Reading values and methods**:
  - `pop_front()` and `pop_back()` return `T?`. When `T` itself is nullable they return `T??`, and an empty array gives the outer `None`;
  - `pop_at(i)`, `front()`, `back()`, `min()` and `max()` return `T`. An empty array or an out-of-bounds index is a program error. An ordinary index out of bounds is a program error too;
  - `Dictionary[K, V].get(k)` returns `V?`, which is `None` for a missing key. `get(k, default)` returns `V`, and `d[k]` with a missing key is a program error;
  - all methods work by the elements' own `==` and `<`. `resize` fills in the default value of the elements, and an empty typed container for an inner container. `duplicate(true)` deep-copies the containers inside the elements;
  - when the elements have no `<`, a sorting method is a compile error. On `Array[Node]`, for example, it suggests `sort_custom`. **[Known limitation]** When `Variant` elements are sorted, a pair that cannot be compared counts as not less, as in GDScript. When `min` or `max` meets such a pair, it returns `null`, also as in GDScript. When the elements have no default value, `resize` is a compile error. On `Array[Node]`, for example, it suggests `Array[Node?]`;
  - the standard library provides `map`, `filter`, `sort` and so on for the native containers, taking typed lambdas.
- **Dictionary keys**: inserting a new key stores a copy. Later changes to the original value do not affect the key. The containers in the copy are read-only at every level. Containers inside the copy are shared as in the original value, including a container that refers to itself. `keys()`, `for k in d` and `find_key()` hand out that read-only copy, and modifying it is a program error. The diagnostic suggests inserting a modified `duplicate()`. A lookup neither copies nor freezes its argument. Suppose this language obtains a container key of a `Dictionary` passed in from outside. That key is then made read-only in place, at every level.

### 3.9 Interfaces

- Interfaces are nominal: a class must declare them explicitly with `implements`, and only classes of this language can implement interfaces. A member that implements an interface must be marked `pub`, and a method cannot be `static`.
- An interface may declare methods, properties and signals, which are all requirements on the implementing class. An inherited engine property can satisfy a property requirement of the same type. A property not marked `@readonly` is required to be readable and writable. A readable and writable member can satisfy `@readonly`, while an engine property without a setter can satisfy only `@readonly`. Using `@readonly` outside an interface is a compile error.
- A signal of an interface is a requirement, as a property is. Signals of the same name that the class declares, inherits or has from an engine class all satisfy it. A signal declared in this language satisfies it only with `pub`. The parameter types of the satisfying signal must be exactly the same.
- An interface may `extends` several parent interfaces. A child interface may redeclare a method of a parent interface with a covariant return type. Members of the same name in parent interfaces with incompatible signatures are a compile error. So is inheritance that forms a cycle.
- **[Known limitation]** In the first version, interfaces have no default methods and no stateful mixins, and cannot be generic. Declaring `interface Source[T]:` is a compile error.

### 3.10 Generics

- The first version has only the built-in generic types: `T?`, `Array[T]`, `Dictionary[K, V]`, `Result[T, E]`, `Task[T]`, `Settled[T]` and `PackedScene[T]`. User code cannot declare type parameters.

### 3.11 Function types and Callable

- A function type contains the parameter types, the return type (`void` when omitted) and whether it is `async`. A call is written `f.call(x)` and is checked statically. Writing `f(x)` is a compile error that gives `f.call(x)`.
- A position that the engine calls accepts an `async` function and runs it as a root task. These positions are signal connections in code and in scenes, and engine `Callable` parameters such as `tween_callback`.
- A method value or a lambda handed to the engine becomes a `Callable` that points directly at the target method. It is not looked up by name. The type of `f.bind(a, b)` is the function type without the last two parameters.
- A `Callable` that the engine passes into a position of function type has an unknown signature. Each call checks the arguments and the return value. **[Known limitation]** The first version cannot explicitly convert a value of static type `Callable` to a function type. It can be called with `call()`.
- A method value does not keep its receiver alive, like the engine's `Callable`. After the receiver is freed, calling the method value is a program error.
  - when the receiver is a RefCounted object newly created by `T.new()`, it is a compile error;
  - a receiver held only by a temporary value stays alive at least until its statement ends, as in `g(make().hit, h())`.
- A lambda captures by value, as in GDScript. It differs in two points:
  - assigning inside a lambda to a captured variable, or to a value-type field of one, is a compile error. Shared mutable state must be kept in a member or a container. Because a capture cannot be reassigned, a type narrowed outside stays narrowed inside the lambda;

  ```gdscript
  var hits := 0
  area.body_entered.connect(func(body): hits += 1)        # compile error: assigns to a captured variable
  var counter: Array[int] = [0]
  area.body_entered.connect(func(body): counter[0] += 1)  # legal: the container is shared
  ```

  - a lambda parameter cannot have a default value, and writing one is a compile error.

### 3.12 Weak references

- **[Known limitation]** The first version has no typed weak references. Where one is needed, use the engine's `weakref()` and `WeakRef`, as in GDScript.

### 3.13 Variant

- **Literals**: an array or dictionary literal that enters a `Variant` position stays untyped. Elements of untyped containers are such positions. An array or dictionary literal without a target type takes its type from its elements, as `var a = [1, 2]` gives `Array[int]`.
- Only `==` and `!=` can be applied directly. All other operations are compile errors, with a quick fix that gives a conversion or `is`. They are arithmetic, `v.x`, `v.f()`, `v[i]` and `for x in v`. Dynamic access by name must first convert to `Object`, and then call `get`/`set`/`call`.
- `Variant` includes NIL. `v == None` is true for NIL and freed objects, `v is int` is false for NIL, and `v as Node` gives `None` for NIL. `Variant?` has one more level of `Option`. For example, `Dictionary[String, Variant].get("hp")` gives `None` for a missing key, and `Some(NIL)` for a `null` value.

  ```gdscript
  var data = JSON.parse_string(text)   # Variant
  data.hp                              # compile error: must be converted first
  if data is Dictionary:
      var hp = int.try_from(data.get("hp", 100)).unwrap_or(100)
  var n = data as Node                 # Node?
  ```

- Data with a fixed structure is decoded into a struct. The whole tree is checked once at the entry, and an error states the field:

  ```gdscript
  struct Consumable:
      var rest_times: int
      var transformed: bool

  struct InitMessage:
      var consumables: Dictionary[String, Consumable]
      var level: int = 1

  var message = InitMessage.try_from(JSON.parse_string(text))   # Result[InitMessage, ConversionError]
  ```

### 3.14 Assignability

- A position that expects type `T` accepts only values assignable to `T`. These positions include assignments, argument passing, return values and container elements.
- Only the following kinds of values are assignable to `T`:
  - a value of the same type;
  - objects: a subclass is assignable to a base class, and an implementing class or a child interface to an interface. Non-null is assignable to nullable, but not the reverse;
  - `None` is assignable to `T?` or to `Variant`, where it gives NIL. When `null` is assigned to a non-null `T`, the quick fix changes the type to `T?`;
  - a value assignable to `T` is also assignable to `T?`. It is wrapped implicitly in `Some`, with levels added to fit the target type. A `T?` assigned to `T??` gives `Some`;
  - implicit conversions. Of GDScript's, only those that lose no information and keep the meaning remain:
    - `int` to `float`, and a value of an enum without payloads to `int` or `float`;
    - `Vector2i`, `Vector3i`, `Vector4i` and `Rect2i` to the corresponding float types;
    - `String` to and from `StringName` and `NodePath`;
  - function values: both `async` or both ordinary functions, with contravariant parameters and a covariant return. A function value is also assignable to `Callable`;
  - `PackedScene[S]` to `PackedScene[T]`, where `S` is assignable to `T`;
  - a typed container whose elements the engine can represent, to the untyped container of the same kind. This gives an untyped view of the same container;
  - any value to `Variant`, where structs and enums with payloads are encoded by the rules of the engine boundary. There are two exceptions:
    - a container whose element type exists only in this language, which must be written explicitly as `Array(hits)` or `Dictionary(d)`;
    - `T??` and `Variant?`, because `None` and the inner NIL cannot be told apart. The quick fix offers `.flatten()` or `.unwrap_or(null)`.
- Every other case needs an explicit conversion. Without one it is a compile error, and the quick fix adds the conversion. For example:
  - typed containers, `Result` and `Task` are invariant in their type parameters, so `Array[Node2D]` is not assignable to `Array[Node]`;
  - `float` to `int` is written `int(x)`, and likewise `Vector2i(v)`, `bool(n)` and `Color(s)`;
  - an untyped container or a `Variant` assigned to a typed position needs an explicit conversion or narrowing through `is`.

## 4. Declarations

### 4.1 Classes and inheritance

- Class names use the engine's flat global class table. The compiler checks for names that duplicate class names of this language, of GDScript plugins and of C#. **[Known limitation]** The first version has no namespaces.
- A class can extend an engine class or a class of this language. **[Known limitation]** Cross-language inheritance is not supported in either direction.
- **Bare-name resolution**: a bare name in a class is first looked up in local variables. Next come the declarations of the class and of its base classes of this language. Last come the members of the inherited engine class and its ancestors:
  - properties, methods, signals and constants;
  - enum values, whose type is that engine enum;
  - enum names, which can be used in type annotations. Bitfield values and the other integer constants are `int`.
- A class declares variables, constants, signals, anonymous enum values and inner classes, which may clash with its engine base class. When one has the name of a constant, enum value or enum name there, it is a compile error. In an inner class, a bare name can be both an outer class's name and one of those engine names. That is a compile error, and `Outer.X` or `Area3D.X` must be written.
- `new` is the constructor name that the compiler provides for a class. Declaring a function named `new` in a class or an interface is a compile error.
- **`@abstract`**: an abstract method has no colon and no body. A class that has abstract methods must be marked `@abstract`. A concrete subclass must implement all abstract methods. An abstract class cannot be constructed, and cannot be attached to a scene node (an error marked on the scene). Calling an abstract method through `super` is a compile error, and so is marking a `static` method `@abstract`.

  ```gdscript
  @abstract
  class_name BattlerAction extends Resource

  @export pub var label: String
  @abstract pub async func execute()
  ```

- **Inner classes**:
  - an inner class may declare everything a file class can, including nested inner classes. But `class_name`, and class-level annotations other than `@abstract`, belong to the file class only;
  - it may extend an inner class of the same file, written by bare name. It may also extend an inner class of another class (`Other.Inner`), or an outer class. When `extends` is omitted, it extends `RefCounted` by default;
  - **[Known limitation]** two files cannot each extend a class of the other, not even indirectly through other files. Otherwise it is a compile error. For example, with `A.X extends B.Y`, a class in B cannot also extend a class of A;
  - a name is looked up first in the class itself and its inheritance chain. It is then looked up in the non-instance declarations of the outer classes, from the nearest outward. Non-instance declarations are constants, types, static functions and static variables. Accessing an instance member of an outer class is a compile error, which explains that there is no outer instance;
  - it can be accessed through `Outer.Inner` or through an outer instance, such as an autoload;
  - at run time it is a separate script with no resource path and no global name, which `get_script()` returns. It cannot be attached directly to a scene node, but can be set through `set_script()`.

### 4.2 Visibility

- A member without `pub` is visible in its declaring file and in subclasses, and not visible to other classes. The same applies to structs, enums, interfaces and inner classes. The declaring file includes the inner classes in it. A subclass can read, write, call and override these members. A member with `pub` is visible to all code.
- An override must keep the visibility unchanged. A member that implements an interface must be `pub`. A non-`pub` type in the signature of a `pub` member is a compile error.
- The parent interfaces of a `pub` interface must be `pub`, or it is a compile error.
- The visibility of `new(...)` is that of the nearest `_init` on the inheritance chain. With no `_init`, `new()` can be called anywhere. When `_init` is without `pub`, only its file and subclasses can call `new(...)`.
- A member that must be assigned and is without `pub` can be assigned only by its file and by subclasses. When none of these files has code that assigns it, it is a compile error at the declaration. The section "Members exposed through reflection" says which members reflection by name can see.

### 4.3 Member variables

- A `var` with an initializer may have its type inferred by the rules of local variables. For example, `var speed = 5.0` is `float`, and `@onready var label = $Label` is inferred from the scenes. The inferred type is used for hover, completion and reflection. Without an initializer, the type must be annotated. It must also be annotated when the initializer cannot determine the type, such as an empty container or `None`.
- A member without an initializer:
  - numbers, `bool`, strings, core value types, containers, `Callable` and `Signal` take their own default values. So does an enum without payloads that has the value 0. `T?` takes `None`, and `Variant` takes NIL;
  - a struct takes its default value when all its fields have default values. Otherwise it is a member that must be assigned. Non-null object references, enums with payloads, enums without 0 and function types are members that must be assigned too.
- `@export` follows GDScript, with three differences. An exported variable must have `pub`. An exported scene is written `PackedScene[T]`. A non-null object export must be assigned.
- Structs and enums with payloads can be exported, and are stored encoded by the rules of the engine boundary. The values stored in scenes and `.tres` files are checked against the type at compile time.
  - When a field contains `Node` at any level, the export is a compile error.
  - **[Known limitation]** For now the Inspector edits them with the generic Dictionary editor.

### 4.4 Property accessors

- Inline `set(value):` and `get:`, and named `set = f` and `get = g`, follow GDScript. Inside an accessor, the property name refers to the backing field. Reads and writes elsewhere in the class all go through the accessors. An initializer does not go through the setter. Setters and getters cannot be coroutines.

### 4.5 Constants

- `const` may be declared in a class or in a function. A class constant is used by its bare name in its class, in subclasses and in static functions. Other classes write `Player.MAX`. It may also be written through an instance, as `self.MAX` or `p.MAX`. An omitted type is inferred by the rules of member variables, so `const SPEEDS = [1.0, 2.0]` is `Array[float]`. When the value is `None`, `T?` must be written.
- The initializer must be a constant expression, with no method calls, lambdas, `self` or variables. A constant expression is made of the following parts:
  - literals, constants, enum values, and constants of engine classes and built-in types;
  - `preload`, operators and conditional expressions;
  - array and dictionary literals, and constructions and conversions of built-in types;
  - the engine's math utility functions;
  - indexes and fields of constants.
- **Division of evaluation**:
  - the compiler computes at compile time `int`, `float`, `bool`, the three kinds of strings, and their operations and conversions;
  - the compiler also computes the math functions that return `int`/`bool`, and the structure of container literals;
  - constructions and operations of engine value types (vectors, `Color` and so on) are evaluated once when the class loads;
  - the math functions that return `float`, and `**` involving a `float`, are also evaluated once when the class loads.
- **Compile-time value checks** (in the table, "known" means computed by the compiler; this guarantees that load-time evaluation never fails):

  | Case | Compile-time requirement |
  |---|---|
  | `/` and `%` of `int` and integer vectors, `posmod` | The divisor (each component of a vector) is known and not 0 |
  | `<<`, `>>` | Both sides are known and non-negative, and the shift amount is less than 64 |
  | `**` of two `int` | Both sides are known; when the exponent is negative, the base is not 0 |
  | `float` to `int` (`int(x)`, `floori(x)` and so on) | `x` is known, is not NaN or infinite, and is within the `int64` range |
  | An index or key taken from a constant | The index or key is known, the index is in bounds and the key exists |
  | Keys of a dictionary literal | Known and not repeated |
  | `Color(s)` | `s` is a hexadecimal literal. A color name is a compile error, and the quick fix changes it to a constant such as `Color.RED` |

- Annotation arguments, enum values and the positions that the table requires to be "known" accept only values the compiler computes. **[Known limitation]** An `int` rounded from a load-time `float`, such as `floori(sin(X) * 100)`, cannot be used in these positions. A constant expression also cannot use:
  - `%` formatting, `is`, `as`, `in`;
  - GDScript's own utility functions (`len`, `char`, `ord`);
  - the `Basis`/`Quaternion` constructions and multiplications whose argument values debug builds check.
- Arrays and dictionaries in a constant are read-only at every level. Assigning directly or calling a modifying method is a compile error, and modifying through another variable is a program error. In hot reload, changing a constant is handled as a structural change.

### 4.6 Functions

- Function signatures are not inferred. Parameters must be annotated with types, except in overrides and in lambdas with an expected type. In a function that is not an override, an omitted return type means `void`. Returning a value is then a compile error.
- **Default values**: a parameter may have a default value. When an argument is omitted, the called function evaluates the default value at its start, each time. The default value can read `self` and the earlier parameters.
  - When an overriding method is called through the base class, the overriding method's own default values are used.
- **Overrides**: an override of an engine virtual function or a base class method may omit parameter and return types. An omitted type is taken from the overridden declaration, as `delta` is `float` in `func _process(delta):`. Written types allow a covariant return and contravariant parameters, and parameter names may differ.
  - An override must accept every argument count that the overridden method accepts. So it may add parameters with default values or a rest parameter, but cannot drop a rest parameter.
- **Rest parameters**: a rest parameter must be the last parameter, without a default value, written `...args: Array` or `...args: Array[T]`. The call site checks the arguments one by one, and a call from outside is validated by the entry check.
  - Outside an override, a bare `...args` is a compile error.
  - There is no spread form at the call site; an existing array uses `callv`.
  - A method value or a lambda with a rest parameter is a `Callable` with an unknown signature.
- An ordinary call passes arguments by position. Named arguments are used only in two places. They construct structs and enums with payloads, and they give `instantiate(...)` and `new(...)` the members that must be assigned.
- A local variable may have its type inferred from its initializer. When there is an expected function type, a lambda parameter takes its type from it. Otherwise it must be annotated. The return type of a lambda is inferred from its body.
- Within one function, some names cannot repeat a local variable or parameter of an enclosing block. They are local variables, `for` variables and the `var x` bindings of `match`. Sibling blocks may use the same name. A lambda body counts as an inner block of the enclosing function. But a lambda parameter may have the name of an outer local variable.
- A coroutine, lambdas included, must write `async func` in its signature.

### 4.7 Signals

- Signal parameters must be annotated with types, as in `pub signal died(cause: String)`.
- `emit` and `connect` in code, and signal connections in scenes, are all checked statically for argument count and types. Engine signals are typed from ClassDB, and unaudited object parameters are taken as non-null.

### 4.8 Static members

- No static initialization is done inside the editor. When classes load, the base class comes before the derived class. Each `static var` is initialized in declaration order, without going through its setter. Then `static func _static_init()`, which has no parameters and no return value, is called. After that the inner classes are initialized in order.
- A `static var` is shared by the class that declares it and its subclasses, and may have accessors. It cannot be marked `@export` or `@onready`. Without an initializer, it takes a default value by the rules of member variables. A type with no default value, such as a non-null object type, is a compile error. Unless marked `@static_unload`, a class with static variables stays loaded during the run.
- Static variables and static methods are used by bare name in their class, in subclasses and in inner classes. Other classes write `Registry.count` and `Base.label()`. They can also be accessed through an instance or a value:
  - GDScript can read and write a `pub` static variable by name, through the class and through objects;
  - objects of classes of this language and of GDScript go by their dynamic type. The static method of that name is looked up from that type upward;
  - on engine classes and built-in types, the method of that type is called directly.

  ```gdscript
  var b: Base = Sub.new()
  b.label()                             # Sub.label()
  Base.label()                          # Base.label()
  var hue := Color.RED.from_hsv(0.5, 1.0, 1.0)
  ```

- Static initialization cannot `await`. Reading a static member of another class first completes the initialization of that class. Directly reading a static variable declared later in the same class is a compile error. Through a call, reading or writing a not yet initialized static variable of the same class is a program error. A cycle of static initialization across classes is a program error too, and the diagnostic lists the dependency chain.
- In hot reload, adding, removing or changing a static member declaration requires running again. Changing only function bodies, initializers or `_static_init` keeps the current values of static variables, and static initialization is not rerun.

## 5. Expressions and statements

### 5.1 Operations

- A negative left operand of a shift is a program error. So is a shift amount that is negative or reaches 64. In a constant these are compile errors.
- In `x in c`, `x` must be assignable to the element or key type. For example, `1 in scores` is legal for an `Array[float]`, and `1.5 in ids` is a compile error for an `Array[int]`. `name in obj` on a freed object is a program error.
- `**` of two `int` is computed exactly, and wraps on overflow as `*` does. A negative exponent truncates toward zero as `1 / (a ** -b)`: `2 ** -1` gives 0, and `(-1) ** -3` gives -1. `0` to a negative power is a program error.
- In `%` formatting, a format string that does not match the values, such as `"%d %d" % [1]`, is a program error.
- Assigning to a character of a String, such as `s[i] = c`, is a compile error. To delete and replace characters, use `erase` and `insert` instead.
- Suppose a value is converted to text, as by `str`, `print` and `%`. It is first encoded by the rules of the engine boundary. The engine then converts it. So a struct is shown as a dictionary, and `None` as `<null>`. A value in a program error is shown as written in this language, such as `Hit(damage = 3)`.

### 5.2 Comparison

- `!=` is always the negation of `==`. `==` differs from GDScript as follows:
  - objects compare identity by ObjectID, and two different freed objects are not equal. Comparing two class types of which neither is a subclass of the other is a compile error. It is allowed when an interface is involved;
  - `Task[T]` also compares by identity: only the same task is equal. It can be a `Dictionary` key;
  - `Option`: `None == None` is true, and two `Some` compare their inner values. The side with fewer levels is first wrapped to the same level. Examples are `int?` with `3`, and `T?` with `T??`. Comparing a value that is neither `Option` nor `Variant` with `None` is a compile error. The quick fix changes it to `T?` or to `is_instance_valid(x)`;
  - containers compare element by element or key by key. They do not match when the element types differ, so `[1] == [1.0]` is false. **[Known limitation]** The first version has not unified how containers with `NaN` match in `==`, lookups and dictionary keys. The result varies with the element type. Comparing containers whose static element types differ, such as `Array[int] == Array[float]`, is a compile error. Structs and enums with payloads match field by field;
  - for a `Variant`, `v == None` is true on NIL and on a freed object.
- Ordering comparisons:
  - when its elements have `<`, an `Array` compares in lexicographic order. At the first position that does not match by the container `==`, the result is the elements' `<` there. When every position matches, the shorter one is less;
  - when `T` has `<`, `T?` makes `None` less than every `Some`, and does **not** wrap implicitly. With `hp: int?`, `hp < 3` is a compile error;
  - structs and enums with payloads are ordered by the rules of the sections on struct and enum;
  - for these three kinds, `>`, `<=` and `>=` derive from `<` and `==`: `a > b` is `b < a`, and `a <= b` is `a < b or a == b`. For other types, all four operations follow the operator table.

### 5.3 Conversions

- `x as T` is used only for object types and returns `T?`. It gives `None` on a mismatch, on a freed object, and on a non-object inside a `Variant`. When the static type of `x` is already `T` or a subclass of it, the result is `T`. A hint then says the cast is redundant. A downcast assertion is written `(x as T).unwrap()`. For code that calls `is_in_group` and then casts, the quick fix suggests `is` instead.
- `as` to a built-in type or to an enum is a compile error. The quick fix offers two changes. One is `T(x)`, which keeps the construction or explicit conversion, such as `int("5")`, `int(2.7)` and `State(x)`. The other is the strict conversion `T.try_from(x)`.
- `T.try_from(v)` returns `Result[T, ConversionError]`, by the same rules as the decoding of struct fields. The runtime type must match `T`. A conversion to `int` accepts a `float` that is exactly an integer and within the `int64` range. A conversion to an enum of this language returns `Err` for a value outside its range of values.
- `ConversionError` is a built-in struct whose only field is `message: String`. Its content is the same as the error reported when that value fails the entry check, such as `Hit.damage: expected int, found String`.

### 5.4 Narrowing

- Local variables and parameters narrow after `!= None`, after `is` and after an early return. They also narrow by the condition in the branches of `a if c else b` and on the right side of `and`/`or`. After a reassignment, narrowing is recomputed from the new value. A lambda capture keeps the narrowing of the place where the lambda is created. Narrowing does not remove the runtime liveness check of objects.
- A `Callable` local variable or parameter that is initialized or assigned with a function value narrows to that function type. A call to it is then checked statically by the function type. On an error, the call belongs to the same call chain as the caller.
- Member variables do not narrow, and must first be read into a local variable.
- There is no truthiness test: conditions, and the operands of `not`, `!`, `and` and `or`, must be `bool`. A non-`bool` `if x:` / `if not x:` is a compile error. The quick fix changes it by type to `x != None`, `x != 0`, `x != ""` and so on.

### 5.5 Conditional expressions

- There is only `a if c else b`, where `c` must be `bool`.
- When there is an expected type, both branches are checked against it. When there is none, the result type is the first of these that applies:
  1. when one branch is assignable to the other, the wider branch;
  2. when one branch is `None` and the other is `T`, `T?`;
  3. when both branches are classes, the nearest common base class.
- When none applies, it is a compile error that suggests writing the type. Two classes that implement the same interface but have different base classes give the common base class. To get the interface, the type must be written.

### 5.6 match

- A value pattern is a literal, a constant expression, an identifier or an attribute access (`A.B`), as in GDScript. A non-constant pattern is compared at run time and does not count toward exhaustiveness.
- A value pattern must have the same type as the static type of the matched value. Otherwise it is a compile error. The following cases are excepted, consistent with what GDScript can match:
  - when the matched value is `Variant`, any type is allowed;
  - `String` and `StringName` match each other;
  - when the matched value is an object, the type of the pattern may be a subclass of its type;
  - when the matched value is `int`, the pattern may be a value of an enum without payloads.
- `var x`, `_`, array and dictionary patterns with `..`, multiple patterns separated by commas and `when` guards are as in GDScript. Array and dictionary patterns have these further rules:
  - an array pattern is used only for `Array[T]` and `Variant`, and a dictionary pattern only for `Dictionary[K, V]` and `Variant`. Using one for another type is a compile error;
  - for the patterns of their elements and values, the matched value has the element type or the value type. For example, in `[var x]` on `Array[int]`, `x` is `int`;
  - a key of a dictionary pattern must be a constant expression. As with `in`, it must be assignable to the key type.
- New patterns:
  - enum variants and structs: `Command.MOVE(target = pattern, ..)`, `Hit(damage = var d, ..)`. `var field` is short for `field = var field`, fields not listed require `..`, and patterns can nest;
  - built-in constructors: `Ok(var v)`, `Err(var e)`, `Some(var x)`, `Finished(var v)`, `Cancelled(var r)`, written by position and without a type prefix;
  - `Option` also has `None`, and `var x` narrowed to `T`. `None` together with `Some(var x)` or `var x` is exhaustive.

  ```gdscript
  match cmd:
      Command.IDLE:
          stop()
      Command.MOVE(var target):
          walk_to(target)
      Command.ATTACK(target = var enemy, power = 0):
          taunt(enemy)
      Command.ATTACK(var target, ..) when target.is_in_group("bosses"):
          flee()
      Command.ATTACK(..):
          hit()
  ```

- A `match` on an enum of this language, `Result`, `Option` or `Settled` must be exhaustive or have `_`. On an open engine enum it must have `_`. A branch with `when` does not count toward coverage. Other types are not required to be exhaustive. An exhaustive `match` on an enum of this language counts as covering all return paths.
- **[Known limitation]** The first version has no type patterns (use `if x is T`) and no `match` expressions. Multiple patterns separated by commas cannot bind.

### 5.7 Loops

- The type annotated on a loop variable must be the element type or a supertype of it. Iterating over a `float` or a vector is a compile error, and the quick fix changes it to `range(...)`.
- The arguments of `range(...)` must be `int`, and the result is `Array[int]`. When `for` iterates over it, it only counts and builds no array.
- A step of 0 in `range(...)` is a program error, in a `for` as well. When the step is known to be 0, it is a compile error.
- When a class of this or another language defines `_iter_init`, `_iter_next` and `_iter_get`, its objects can be iterated over. The element type is the return type of `_iter_get`. When any of the three is missing, it is a compile error.
- These three methods are virtual functions of Object, and are called by the engine's protocol:
  - each time, `_iter_init` and `_iter_next` receive a new single-element `Array`. The element is the iteration state, which is `null` at the start;
  - when either returns `true`, `_iter_get` is called with the state at that time to get the next element. When either returns `false`, the iteration ends;
  - when either returns and the length of this `Array` is not 1, it is a program error.

## 6. Initialization and lifecycle

### 6.1 Initialization of members

- The signature of a subclass's `_init` is not bound by the override rules.
- The initializers of instance and static members write the underlying field directly, in declaration order, without going through the setter. A member with both an initializer and a setter gives a hint. Directly reading a member declared later in the same class is a compile error. Derived state is computed in `_ready` or `_static_init`.

### 6.2 Members that must be assigned

- `_init` may have parameters, and `new(...)` passes arguments to it by position, as in GDScript. When the engine creates an object, it passes no arguments, as with scenes, `instantiate(...)` and `duplicate()`. For these routes, construction parameters are expressed as members that must be assigned, non-null and without default values.
- Before its scene finishes becoming ready, a `Node` member that must be assigned must be provided by one of these:
  - **`_init`**: assigns it on every path, for example from a parameter;
  - **the scene**: an exported member is provided by the scene;
  - **the creator**: `instantiate(...)` and `new(...)` take members by name and assign them before returning, as in `BulletScene.instantiate(shooter = self, target = enemy)`. In `PackedScene[T].instantiate(...)`, leaving out a member that the scene resource rules require is a compile error. A member left out of `new(...)` is checked at run time;
  - **an ancestor**: an ancestor node in the same scene assigns it in its own `_ready`. The assignment goes through `$Child.target = v` or an `@onready` reference, which the scene model can see. Dynamic configuration that it cannot see must be marked `T?`.
- The compiler checks the values given in the following forms, and other assignments are checked at ready:
  - assignments on every path in `_init`;
  - values stored in the scene;
  - named arguments of `new(...)` and `instantiate(...)`;
  - assignments by an ancestor in `_ready`, through `$Child.target = v` or an `@onready` reference.
- A member still unassigned at ready is a program error. The error suggests one of the forms above, so that it is checked at compile time.
- The ready order says when the checks happen. A child node reading an ancestor-assigned member in its own `@onready` initializers or `_ready` is a compile error. Before ready, reading a member not yet assigned is a program error that states the member name.
- Suppose `_init` directly reads a member that must be assigned. Unless `_init` has assigned it on every path before that, the read is a compile error. Reading it indirectly through a call is a program error.
- A `Resource` has no ready phase. A `.tres` is checked statically, and one made by `new()` in code is checked when read.
- When `_init` has required parameters, attaching the script by a route without arguments, such as `set_script`, is a program error. `_init` is not run then. A scene or a `.tres` that uses such a script is a compile error.
- An autoload outlives scenes, so its members that refer to scene nodes must be written `T?`. **[Known limitation]** The first version has no non-null members assigned after ready.

### 6.3 `@onready`

- Using `@onready` together with `@export` is a compile error. So is an `await` in the initializer, and so is directly reading an `@onready` member in `_init` or `_enter_tree`. Reading an `@onready` member not yet initialized, indirectly through a call, is a program error that states the member name.

### 6.4 Ready order

- A `Node` of this language goes through the following phases in order, with each check done in the phase listed:
  1. Creation: the initializers assign in declaration order, and then `_init` runs. The named arguments of `new(...)` and `instantiate(...)` are assigned next, in written order and through the setters. This is the same as a scene assigning exports;
  2. Parenting (`NOTIFICATION_PARENTED`): `@parent(T)` is checked. When the script is attached to a node that already has a parent, it is checked on attaching;
  3. Ready: first the members that must be assigned and that the scene or the creator provides are checked. Next the `@onready` members are initialized, and then `_ready` runs;
  4. Whole scene ready, that is when the owner emits `ready`: the ancestor-assigned members that must be assigned are checked.
- On a node that has an owner in the scene, an ancestor assigns the unexported members that must be assigned. The rest are provided by the scene or the creator. When an ancestor assigns only after an `await` in its `_ready`, the check of step 4 is a program error.
- Suppose a child node reads, in `_ready`, a member of its parent that is not yet initialized. That is a program error, which explains the cause. The read is legal when the child is added under an already ready parent at run time. So it is not forbidden outright at compile time. The diagnostic gives two fixes:
  - let the parent's `_ready` configure the child instead;
  - the child declares `async func _ready()` and `@parent(T)`, and writes `await get_parent().ready`.

### 6.5 Setters before ready

- A setter runs immediately. Before ready, accessing an `@onready` member or a child node is a program error. It states the member or node name. A setter that needs to sync a node should store the value first, and sync when `is_node_ready()` is true. It should also sync once in `_ready`. Writing `await ready` inside a setter is a compile error.
- Suppose a property has no backing field, and its getter reads a child node directly. Reading it before ready is a program error that states the node that is not ready. A property never written does not run its setter at ready. An exported property whose default value does not go through its node-syncing setter gives a hint.

### 6.6 Destruction

- During `PREDELETE`, a `RefCounted` can read and write its own fields and call its own methods normally. No reference is added again. In the branch that handles `PREDELETE`, using `self` as a value is a compile error. Use as a value means passing as an argument, storing, returning, comparing and so on, except `self.x` and `self.f()`. Using `await` in that branch is a compile error too.

### 6.7 Reference cycles and leaks

- The compiler gives a warning for an obvious reference cycle:
  - a closure that captures `self` is stored in a member, signal connection or container of `self`;
  - two `RefCounted` classes refer to each other through strong reference fields.
- Consider a `Node` created by `new()` / `instantiate()`. When some path neither adds it to the tree, returns it, stores it nor frees it, it gives a warning. When the result of `remove_child` is neither stored nor freed, it gives a warning too.
- At exit, a debug build lists the instances of this language that are still alive. It also lists the objects created by `new()` of this language.
  - RefCounted objects that refer to each other in a cycle are included.
  - They are grouped by creation site. Each group states the class and the count, and at most 5 sites are listed. Instances not created by code of this language form a group of their own.
  - The list is best effort: everything listed has really leaked, but the list is not guaranteed to be complete. As detection improves, the range of what is listed may grow.

### 6.8 Release timing

- Temporary values and local variables release the references they hold by scope, not early after their last use. So guard forms of the kind that unlock on destruction work:

  ```gdscript
  func save():
      var guard := FileLock.new(path)   # released only when the function returns
      write_all()
      if load_config().verbose:         # the result of load_config() is released before entering the block
          log_all()
  ```

- A temporary value is released at the end of its statement. The header of a compound statement is an exception:
  - the temporary values of an `if`, `elif` or `while` condition are released right after the test. So are those of the matched value of `match`. They are not carried into the block;
  - the value that `for` iterates over is kept until the loop ends. Other temporary values of the header are released before entering the loop body.
- A local variable is released on leaving its block. The end of each round of a loop body also counts as leaving.
- When an `await` in a statement suspends, the temporary values of that statement are still held. `self` is excepted, as the release rules of coroutines say.
- GDScript releases values that code can no longer reach later than this. Examples are the loop locals of the previous round and the matched value of `match`. This falls under the exception to the syntax hard rule.

## 7. Scene model

### 7.1 Typing nodes

- The compiler collects every scene structure that the script is in, including inherited scenes and changes to instances. From them it types `$X`, `%X` and `get_node` with a literal path.
- When different scene structures give different types, it is a compile error. The node must be assigned to a variable annotated with a common base class or interface, such as `@onready var weapon: Node3D = $Weapon`.
- A path that cannot be found is a compile error, marked on the scene that lacks the node. The next section says which scenes must have the node.

### 7.2 Structures that differ between scenes

- **Child nodes that a base class requires and inherited scenes provide**: they are checked by how the scene is used. Exports that must be assigned are checked the same way. A scene used only as a base for inheritance may lack the node. A scene used as a root, or instanced, must have the node. A scene referenced from nowhere that lacks the node gets only a warning. The type of `$X` is taken from the scenes that contain the node.
- **Optional child nodes**: `get_node_or_null(...)` with a literal path has type `T?`. A scene that lacks the node is not an error. When `$X` is missing in some scene that is used, the quick fix changes it to `get_node_or_null`:

  ```gdscript
  @onready var trail_right = get_node_or_null("Container/TrailRight")   # GPUParticles3D?
  ```

- **Child nodes added at run time**: they are not typed by path. A `$X` that no scene has, unreferenced scenes included, is a compile error. The quick fix suggests storing the node in a typed member at `add_child`, such as `var tetromino: Tetromino?`. When a lookup by name is really needed, use `get_node_or_null("X")`, which gives `Node?`, and convert with `as`. A literal `get_node_or_null` path that exists in no scene gives a warning. When it really is a runtime node, turn the warning off with `@warning_ignore`.

### 7.3 Upward access

- An autoload is accessed by its global name, and its type is the class of its script. When `X` is an autoload, the literal path `/root/X` is a compile error. The quick fix changes it to `X`.
- When `X` is not an autoload, `$"/root/X"` is the same as `get_node("/root/X")`. Its type is `Node`, and a concrete type needs an explicit conversion. Code adds such nodes under the root at run time.
- Requirements on the parent and the owner are written as the class-level annotations `@parent(T)` and `@owner(T)`. Once declared, `get_parent()`, `$".."` and `owner` have type `T`. When not declared, `get_parent()` and `owner` are `Node?`, while `$".."` and `get_node("..")` are `Node`. Writing `get_parent().x` or `owner.x` is then a compile error. The quick fix infers a common type from the scenes and adds `@parent(T)` / `@owner(T)`, or adds `.unwrap()` at the call:

  ```gdscript
  @owner(Combat.Combatant)
  class_name HealthComponent extends Node

  func _ready():
      current_health = owner.init_health
  ```

- **Static check**: it is done on the scenes where the script is placed. `@parent(T)` checks the parent inside the scene. When the scene root is instanced into an outer scene, it checks the outer parent. On the main scene, it checks the `Window` root viewport. A mismatch is an error marked on that scene. Other scenes used as a root are checked only at run time. `owner` is the root node of the scene that contains the node. When the scene root is instanced into an outer scene, it is the root of the outer scene. In a scene used as a root, the root node has no `owner`. When that root has a script with `@owner`, it is an error on that scene.
- **At run time**: `@parent(T)` is checked as the ready order says. Calling `get_parent()` with no parent is a program error. `@owner(T)` checks on every read of `owner` that it is non-null and fits `T`. A failure is a program error that states the requirement and the actual `owner`. Nothing is checked when a node is moved or on `set_owner()`.
- `get_owner()` and `get_node_or_null("..")` always have type `Node?`. Code that declares a requirement can use them to test whether the parent or the `owner` exists.

### 7.4 Scene resources

- `preload`, and `load` with a literal path, are typed by the actual content of the resource. A scene gives `PackedScene[T]`, where `T` is the type of the root node or the class of its script. `preload` of a script gives its class.
- An exported scene is written `PackedScene[T]`. The editor checks the scene filled in, and `instantiate()` gives `T`.
- The members that `instantiate(...)` must be given depend on whether the scene is statically known:
  - statically known, which means `preload`, `load` with a literal path, and constants initialized with them. The members that must be assigned and that the scene does not provide must be passed;
  - any other `PackedScene[T]`, which means exports, variables, parameters and return values. The unexported members of `T` that must be assigned must be passed (the scene itself provides the exported ones);
  - a statically known scene can be converted implicitly to the latter kind of `PackedScene[T]`. Examples are filling an export, assignment, argument passing, return, and use as a container element. The conversion looks at the actual class of the root node. It checks the members that must be assigned and that the scene does not provide. They can only be unexported members of `T` that must be assigned. Otherwise it is a compile error at the conversion.
- `load` with a runtime path gives `Resource`, with nullability by the rules of engine APIs. `instantiate()` of such a scene gives `Node`, and a concrete type needs an explicit conversion.

### 7.5 Exports that must be assigned

- An export of a non-null object type must be assigned. One that may lack a value is written `T?`.
- It is checked by how the scene is used:
  - when other scenes instance or inherit the scene, an assignment at any level of the instance and inheritance chain counts. Each instance is checked separately, and marked on the scene it is in;
  - when the scene is used as a root, the scene itself is checked. Such a use is the main scene, `load`/`preload` with a literal path, or a `PackedScene[T]` filled into an export. Values passed to `instantiate(...)` in code count too, and leaving one out is a compile error at the call site;
  - a scene referenced from nowhere gets only a warning.
- The checks happen at three times:
  - **While editing**: clearing is allowed. The Inspector field and the scene tree node are marked with a compile error at once, without blocking the operation. The mark disappears after a new assignment;
  - **At run time**: a missing-value error marked on a scene does not prevent running in the editor. A node that still lacks the value is checked as the ready order says. It is a program error that states the member name;
  - **At export**: a scene certain to lack a value reports an error through the export plugin. The export dialog shows a failure and states the scene and the member name. What is only a warning reports no error. CI blocks with the headless analysis command. **[Known limitation]** The engine exporter does not stop writing its output because a plugin reports an error. The exit code of a command-line export is still 0.
- When the remote inspector writes `null` to a non-null member, the entry check fails, which is a program error. The write is rejected and the old value stays.

### 7.6 Resource sharing

- The semantics of resource sharing do not change. The compiler recognizes resources that come from the cache (`load`, `preload`, exported properties). It also recognizes embedded sub-resources not set to "Local to Scene".
- Writing a property of a resource known to be shared, or calling a modifying method on it, gives a warning. The warning lists the places that share it and offers two quick fixes. One sets it local to scene, and the other calls `duplicate()` before the modification. When the sharing is intended, turn the warning off with `@warning_ignore`. Resources replaced at run time are not tracked.
- `duplicate()` returns the static type of the receiver. The depth of the copy is stated with `duplicate_deep(mode)`.
- A reference cycle among `preload`, exported `PackedScene` and resources is a compile error. The quick fix changes one of its edges to a runtime `load`.

### 7.7 Connections and paths in scenes

- A signal connection saved in a scene is checked. The checks are whether the target method exists and whether its parameters fit the signal.
- Animation property tracks, method tracks and property paths such as `"position:x"` are resolved as static references. They take part in rename and find references.
- When a track or a synchronized property fails to resolve:
  - a node whose class is a class of this language, but lacks the member referred to, is a compile error;
  - when the path finds no node, it gives a warning, because a shared animation library may lack nodes on purpose.

## 8. Errors

### 8.1 Program errors

- A program error aborts the whole call chain of this language. It returns to the entry point where the engine called in. A call to a method declared by a class of this language belongs to the same chain. This holds whatever class the receiver is, even for a call through an interface. Suppose the static type of `f` is a function type. Calling a function value of this language with `f.call(x)` / `f.callv(args)` then belongs to the same chain too. A function value is a lambda or a method value, including after `bind()`. Only an engine entry point starts a new chain. These include signals, engine `Callable`s, engine virtual functions, and `call()` and `callv()` on a value of static type `Callable`. On the way out, held references are released, and the error and the call stack are recorded. When running in the editor, it enters the debugger. The game keeps running.
- A debug build only adds diagnostic detail, and reports leaks at exit.
- An engine entry point that needs a return value gets the default value of that return type after the abort.
- Every case this document calls a "program error" is handled this way. Examples are accessing a freed object, a failed `unwrap`/`expect`, an out-of-bounds index and integer division by 0. Others include reading an unassigned member before ready, failed entry checks, modifying a `Dictionary` key and awaiting a cancelled task.
- On one thread, calls of this language nested more than 1024 levels deep are a program error. They do not crash from stack overflow.

### 8.2 Expected failures

- An expected failure is expressed with `Option`, which may be left unhandled, or `Result`, which cannot be discarded.
- The only special syntax is the postfix `?` for short-circuit return:
  - applied to `Result[T, E]`: on `Err(e)` it returns `Err(e)` from the current function, and otherwise gives `T`. The current function must return a `Result[_, E]` with the same `E`. A different `E` is first converted with `map_err`;
  - applied to `T?`: on `None` it returns `None` from the current function, and otherwise gives `T`. The current function must return `_?`;
  - using `?` in a function that returns `void` is a compile error. So is propagating directly between `Option` and `Result`. Convert with `ok()` and `ok_or()`;
  - precedence makes `await f()?` parse as `(await f())?`.

  ```gdscript
  func load_profile(path: String) -> Result[Profile, Error]:
      var text = read_text(path)?
      return Ok(parse_profile(text)?)
  ```

### 8.3 Assertions

- `assert(condition, message)` is evaluated only in debug builds, which are runs in the editor and debug exports. A failure is a program error. In release builds it is removed, and neither the condition nor the message is evaluated.
- The message is a `String`, evaluated only when the condition is false. This falls under the exception to the syntax hard rule. GDScript always evaluates it in debug builds, and not in release builds. An `await` or a postfix `?` in the condition or the message is a compile error. The condition must be `bool`. For example, `assert(save_data, "...")` is a compile error that suggests `save_data != None`. A check that release builds need too uses `expect` or an explicit `if`.

### 8.4 Diagnostics

- Compile errors, warnings and hints are reported by the analysis service. They are marked in the source code, the Inspector and the scene tree.
- The diagnostic of a program error states where it happened, the member or object concerned and the call stack. It also contains the specific information that each section prescribes. Examples: the name of a member that must be assigned, the free site, the `owner` requirement and the actual `owner`.
- Compile-time and runtime diagnostics both carry their own reasons, and do not cite this specification.

## 9. Coroutines

### 9.1 async and Task

- A function or a lambda that contains `await` must write `async func` in its signature, as in `button.pressed.connect(async func(): await fade_out())`. Without it, it is a compile error, with a quick fix that adds `async`.
- Only engine virtual functions that return `void` can be declared `async`. `async func _init()` is a compile error, and so is an `async` override of a non-`void` engine virtual function. The same holds when the return type is omitted. A position that the engine calls accepts an `async` function and runs it as a root task.
- Calling an `async` function starts it at once, runs it to its first suspension, and returns `Task[T]`. With no return value it is `Task[void]`. `void` can be a type argument only in `Task[void]`. A bare `Task` is a compile error. `await f()` and `await t` give `T`.
- Discarding the result of an `async` call is a compile error. To not wait deliberately, write `f().detach()`.
- Setters, getters, `@onready` initializers and static initialization cannot `await`.

### 9.2 End states

- A task has three end states. **Finished** means it got its return value. **Cancelled** means `t.cancel()` was called, its owning object was freed, or the signal source of `Task.of(signal)` was freed. **Failed** means a program error happened inside it. This includes an `await` on a cancelled task or on a freed signal source.
- An `await` on a cancelled task is a program error. A failure propagates along the `await` chain to the root task. That is the task given to `detach` or called by the engine. A failure cannot be intercepted.
- `await t.settled()` returns cancellation as a value, in a `Settled[T]`: `Finished(v)` or `Cancelled(r)`. `r` is an `enum CancelReason`: `REQUESTED`, `OWNER_FREED`, `AWAITED_FREED`. When the task fails, the failure propagates to the waiter as usual.
- The `Finished` of a `Task[void]` carries no value, and its pattern is written `Finished`.

### 9.3 Cancellation and ownership

- On a suspended task, `cancel()` at once disconnects the signal or task it is waiting for, and cancels it. On a task that is executing, `cancel()` can only come from code that the task calls synchronously. An example is a handler of a signal it emits. The task is then cancelled at its next `await` or return, and its return value is discarded. On a task that has ended, it has no effect. On a task that has not ended, the result after `cancel()` is always cancelled.
- Cancellation propagates along ownership. A child task started and awaited directly with `await f(...)` is owned by the parent task, and is cancelled with it. With an `await` on an existing `Task` value, cancellation only stops the waiting, and the original task keeps running.
- A cancellation caused by the owning object being freed does not propagate along ownership. A child task on another object that is still alive keeps running as if by `detach`, as in GDScript.

### 9.4 Release

- When a task ends, the coroutine frame and the references it holds are released at once. The code after the `await` no longer runs. **[Known limitation]** The first version has no `finally` or `defer`.
- `Task` is a `RefCounted` handle that stores only the result. A suspended coroutine does not keep `self` alive.

### 9.5 Combination and reentrancy

- `Task.of(signal)` wraps a signal as a task. `Task.delay(seconds)` waits for a time. `Task.any(list)` returns the index of the first task to finish, and cancels the other tasks by the ownership rules. `Task.all(list)` waits for all to finish. A task started directly in an argument of `any`/`all` is owned by it. An existing task is only no longer waited for.
- `await Task.all(list)` produces no value. When the results are needed, first store the tasks in variables, and `await` them one by one after it finishes.
- There is no automatic policy for reentrancy. Where needed, cancel the old call through its handle.

### 9.6 Awaiting signals

- An `await` on a signal without parameters produces no value. A signal with one parameter gives a value of that parameter's type. An `await` on a signal with several parameters can only be a statement. Using the result is a compile error.
- A signal accessed through a known class has a known signature. Known classes are those in the engine's ClassDB and the classes and interfaces declared in this language. Classes of other languages seen through reflection are known too. When the static type is only `Signal`, the signature is unknown. An `await` on it as a statement is not restricted. An `await` on it as a value gives `Variant`. At run time that is NIL for no parameters, the value for one, and an `Array` for several:

  ```gdscript
  var r = await DialogueManager.Resolved   # unknown signature: r is Variant
  if r is Array and r[0] == id:
      return r[1]
  ```

### 9.7 Freed objects and suspended coroutines

- When the owning object of a coroutine receives `NOTIFICATION_PREDELETE`, its coroutines are cancelled silently at once. This is before a `Node` deletes its children. The references of the suspended frames are released.
- The awaited object is the host of the awaited coroutine, or the source of the awaited signal. Either one is an object other than the waiter's own. When it is freed before completion, the decision is deferred. It is made in the per-frame callback after this frame's deletion queue is emptied:
  - if the owning object of the waiter has also been freed in the meantime, the waiter is cancelled silently (`OWNER_FREED`);
  - otherwise it is a program error for the waiter at the `await`, stating the freed object and its free site. The error propagates along the `await` chain.
- Waiting through `settled()` is not an error: `Task.of(signal)` gives `Cancelled(CancelReason.AWAITED_FREED)`, and a coroutine of another object gives `Cancelled(CancelReason.OWNER_FREED)`:

  ```gdscript
  # hud.pet: HUD and player are both children of Main
  async func _ready():
      await player.died         # deleted together on a scene change: cancelled silently
      show_game_over()          # player freed alone without emitting died: a program error after this frame
  ```

### 9.8 Cross-language await

- A call to a GDScript method may be written with `await`. It waits for `GDScriptFunctionState.completed` and does the entry check against the declared return type. Without `await`, a call as a statement is not an error. After running to its first suspension, it continues as if by `detach`. Without `await`, a call used as a value that gets a `GDScriptFunctionState` at run time is a program error. The error suggests adding `await`.
- While waiting for a `GDScriptFunctionState`, the call's target object is the receiver, or the target object of the `Callable`. When it is freed or its script is replaced (`script_changed`), the previous section's rules for a freed awaited object apply. `settled()` gives `Cancelled(CancelReason.OWNER_FREED)`. **[Known limitation]** When a GDScript script is reloaded in the editor, the engine silently clears the suspended functions. The waiter then hangs forever.
- When GDScript uses `await` on a coroutine of this language, the first suspension returns the `completed` signal of the `Task`. An `async` method of this language reports an untyped `Variant` return value to the engine. The declared type is used only for documentation and hover. When the task ends:
  - finishing emits `completed(value)`;
  - cancellation first emits `cancelled(reason)` and then `completed(null)`, where the reason is a value of `CancelReason`;
  - failure emits only `completed(null)`. The failure has already been reported as a program error, and cannot be intercepted, as in this language.

## 10. Engine boundary

### 10.1 Types that the engine and GDScript see

| This language | What the engine and GDScript see |
|---|---|
| Objects `T` and `T?` | That class in both cases; non-null is guaranteed by the entry check |
| Nullable value types such as `int?` | Untyped `Variant`, with `None` as NIL |
| `Array[T]` and `Dictionary[K, V]` whose elements the engine can represent | Engine typed containers (a nested container carries the types of its outer level only) |
| struct | A `Dictionary` with `String` field names as keys |
| enum with payloads | A `Dictionary` with `String` field names as keys. The reserved key `"$variant"` stores the variant name: `{"$variant": "MOVE", "target": p}`. A variant without payload is `{"$variant": "IDLE"}` |
| `Result[T, E]` | The same as an enum with payloads: `{"$variant": "Ok", "value": v}` or `{"$variant": "Err", "error": e}` |
| Return value of an `async` method | Untyped `Variant` |

- `T??` and `Variant?` cannot be encoded. The reason is that in the engine the outer `None` and the inner empty value are both null.
  - When such a value must be encoded across the engine boundary, it is a compile error. The same holds for a struct, an enum with payloads or a `Result` whose fields contain them.
  - A function value converted to `Callable` or `Variant` has its parameters and return value checked as for a pub function.
  - The quick fix adds `.flatten()` for `T??`, and `.unwrap_or(null)` for `Variant?`.
- Through script reflection, GDScript checks global classes of this language statically. It checks the instance members (properties, methods, signals) and the positional arguments of `Player.new(...)`. When `_init` is without `pub`, a GDScript call to `new` with arguments is a compile error. A `new` without arguments reports an error at run time. Named arguments for members that must be assigned can be used only in this language. The section "Cross-language calls and messages" covers other class-level access.
- **Entry check**: a value that enters this language is validated against the declared type. It enters from the engine, GDScript, C# or the remote inspector. The routes of entry include parameters, members written by `set`, signal arguments, `await` results and decoding. A failed validation is a program error.

### 10.2 Nullability of objects in engine APIs

- For an engine API not audited by the binding metadata, nullability is set by the direction of flow:
  - a value returned by an engine method, and a property read, are `T?`;
  - the parameters of virtual functions that the engine calls in this language, and engine signal parameters, are non-null `T`. A `null` passed in is a program error from the entry check. An override or a handler may mark them `T?` itself to accept null;
  - method arguments and property writes that this language passes to the engine accept `None`, as in `set_owner(null)`.
- The audit proceeds by how often an API is used. Return values fall into three classes. They are non-null `T`, `T` that is null only when a misuse reports an error, and normally nullable `T?`. An example of the second class is `get_tree()`, where null is a program error. Examples of the third are `get_collider()`, `get_first_node_in_group()` and `get_parent()`. Parameters are audited likewise, as `Area3D.body_shape_entered` is marked nullable.

### 10.3 Binding metadata

- The binding metadata fills in existing constraints that the engine does not export, and exported ones are used directly.
- The `Variant` math utility functions take arguments of one type and return that type. They are `lerp`, `clamp`, `min`, `max`, `abs`, `wrap` and so on. Mixing `int` and `float` is a compile error. The quick fix changes the integer argument to `float`, as in `min(1.5, 1.0)`.
- An engine API that filters by a literal class name is typed by that class. For example, `find_children("*", "MeshInstance3D")` returns `Array[MeshInstance3D]`.
- Some engine APIs return an empty value on failure, and the error can be read afterwards. An example is `FileAccess.open` with `FileAccess.get_open_error()`.
  - For each of them the standard library adds a version that returns `Result[T, Error]`. Its name is the original name with the prefix `try_`, such as `FileAccess.try_open`.
  - The type of the original API does not change.
  - An API whose error cannot be read still returns `T?`, such as `load()`.

### 10.4 Member names in string form

- A literal name in the following positions is resolved as a static reference, the same as writing `target.name`. The positions include `connect`, `emit_signal`, `call`, `call_deferred`, `set_deferred`, `has_method`, `Callable(obj, "m")`, `tween_property` and property paths such as `"position:x"`. The binding metadata marks the parameter positions. These names are checked for existence and value type, and take part in rename and find references. A non-literal name is an explicit dynamic access.
- When the target has no such name, it is a compile error, except in these three cases:
  - the static type of the target is `Object` or `Variant`, which is dynamic access;
  - runtime queries such as `has_method`, `has_signal` and `has_user_signal`;
  - the name contains `/`, as in `"theme_override_colors/font_color"`. The engine lists such properties per instance.
- A name that refers to a member of this language without `pub` is a compile error. The reason is that the engine cannot find it by name. For a method name, the quick fix uses a method value instead: `call_deferred("reset")` becomes `reset.call_deferred()`.
- A name that begins with `@` is a compile error, also when the target is `Object`. Such a name can only be the rewritten name of a private signal.

### 10.5 Members exposed through reflection

- Through reflection by name, the engine, GDScript and C# can access only `pub` members. Reflection by name means `get_property_list`, `has_method`, `call`, `get`, `set` and `connect`. Overrides of engine virtual functions are excepted: the engine calls them by their agreed names. Other members are invisible to access by name. GDScript reports a compile error, and a dynamic access reports at run time that the member is not found.
- The following members, which the engine accesses by name, must have `pub`. Otherwise it is a compile error, and the quick fix adds `pub`:
  - exported variables and `@rpc` functions;
  - the signals and methods of signal connections in scenes;
  - the members that animation property tracks and method tracks refer to, and the properties that `MultiplayerSynchronizer` synchronizes.
- A signal without `pub` is registered with the engine under the rewritten name `@name`. In this language the original name is written as usual, and `get_name()` of the `Signal` value returns the rewritten name. **[Known limitation]** Another language that spells out the rewritten name can still access such a signal by name.
- A container whose element type exists only in this language is not exposed through reflection, even when it is `pub`. A `struct` member is exposed as usual, encoded and decoded by copy.
- A `Callable` formed from a method value, as in `tween_callback(change_weapon)`, binds the target method directly, with no lookup by name. So it can point at a method without `pub`. A `pub` inner class is exposed as a class constant whose value is its script. The debugger can see all members.
- Reading or writing a property of an engine class calls its getter or setter, without going through access by name. `label.text = s` is the same as `label.set_text(s)`.
- So the `_get` and `_set` of an object's script intercept only access by name. Examples are `get`, `set`, the Inspector and animations.
- Properties added by `_get_property_list()` also pass through `_validate_property()`, as in GDScript.
- **[Known limitation]** The engine does not give scripts the index of an indexed property. For example, the textures of `BaseMaterial3D` share one pair of methods. Such properties in classes of other extensions are therefore still read and written by name.

### 10.6 Numeric boundaries

- A value passed to an engine parameter or property of a narrow or unsigned integer type gets a range check. These types are `int8`, `int16`, `int32`, `uint8`, `uint16`, `uint32` and `char32`. A value out of range is a program error.
- `uint64` and `int` convert bit for bit, with no range check. Their values correspond one to one. For example, the instance ID of a RefCounted reads as a negative number, and is passed back unchanged. **[Known limitation]** Classes of other extensions are not in the engine API description. Their narrow integer parameters are not checked.
- A write to a 32-bit float position of the engine narrows implicitly. An example is a `real_t` vector component in a standard build.

### 10.7 String and StringName

- A runtime conversion between `String` and `StringName` or `NodePath` on a hot path gives a performance hint.

### 10.8 JSON

- Suppose a JSON number is decoded to `int` or to an enum without payloads. Only a float that is exactly an integer and within the `int64` range is accepted. Other floats are not truncated: `try_from` gives `Err`, and an entry check fails with a program error.
- Structs and enums are already encoded by the rules of the engine boundary when assigned to `Variant`. So `JSON.stringify`, `var_to_str` and `ConfigFile` store and load them directly, and reading back uses `T.try_from`. Containers of them are first converted with `Array(hits)` or `Dictionary(d)`.
- **[Known limitation]** JSON writes engine values such as `Vector2` and `Color` as strings. When they are read back, the type does not match. Integers whose absolute value exceeds 2^53 also lose precision. There are three ways to store these values:
  - `var_to_str`;
  - `ConfigFile`;
  - conversion through `JSON.from_native` and `JSON.to_native`, still stored as JSON.

## 11. Coexisting with GDScript and C#

### 11.1 The static view of classes of other languages

- Through reflection, this language sees the classes of other languages (GDScript, C# and so on) and autoloads. This includes their subtype relations by inheritance, which apply to parameters, `is` and `as`. Members, parameters and return values of object types are treated as `T?`. Value types are as declared, and what has no type annotation is `Variant`.
- An autoload name is a global identifier whose type is its script's class, also when there is no `class_name`.
- An autoload name can also be written in a type, where it names the same class. Examples are `func get_export() -> Export` and `f: Export.FileFormat`.
  - A scene autoload names the class of the root node's script. When the root node has no script, it names its engine class.
  - A nearer declaration of the same name in scope takes precedence.
- Suppose a class of another language is named by its global class name or by `preload`. It is then constructed with `new(...)`, by the parameters of its `_init`. Static methods and constants of value types can also be accessed through the class (`Unit.make()`, `Unit.TILE_SIZE`).
- **[Known limitation]** The engine does not provide the constructor parameters and constants of C# classes. So a C# class can be constructed only with `new()` without arguments, and its constants cannot be accessed.

### 11.2 Inheritance

- Cross-language inheritance is not supported in either direction:
  - a class of this language cannot extend a GDScript class. When a plugin requires inheritance, write a thin GDScript forwarding subclass by hand. Its calls to instance members of this language are still checked statically by GDScript;
  - when a GDScript script extends a class of this language, the analysis service reports an error on that GDScript file. The error explains the cause;
  - migrating a class must also migrate its GDScript subclasses.

### 11.3 Cross-language calls and messages

- By name, GDScript and C# can access only the members exposed through reflection.
- **[Known limitation]** Besides using instances and calling `new`, GDScript can read and write `pub` static variables by name. It can also read constant values and `Player.Inner` scripts. Beyond that, the following class-level access is not supported. The alternative forwards through an instance method of an instance or of an autoload:
  - calling a static method of this language;
  - using an enum, constant or inner class of this language in a GDScript type annotation or constant expression;
  - typing an autoload written in this language, which GDScript treats as `Node` or as untyped.
- Messages to and from C# are expressed as structs. They are encoded and decoded as a `Dictionary` by field name, with the entry check.

## 12. Runtime

### 12.2 Performance

- The goal is to be clearly faster than GDScript. C# is not the target.
- **[Open]** When native code is generated: at run time, at export, or both.

### 12.3 Threads

- The virtual machine must run safely on any thread the engine schedules it on. Examples are background resource loading threads and physics threads. Data races on shared objects are the user's responsibility.
- **[Known limitation]** The first version adds no thread API. Use the engine's `Thread`, `WorkerThreadPool` and `Mutex` directly.

### 12.4 Hot reload

- When only function bodies change, they are replaced directly during the run. The next call takes the new code. Suspended coroutines and lambdas created earlier keep running the old version of the code that created them.
- When members, signatures, constants or the class structure change, state is not migrated. The running game then prints a message asking to run it again. The current run refuses this and every later hot reload. This guarantees that each class has only one version within one run.

### 12.5 Debugging support

- It plugs into the engine's debugging interface (breakpoints, stepping, local variables, call stack).
- A debug build records the free sites of instances of this language and of objects freed by this language. At exit it lists leaked nodes and objects by creation site.
- **[Known limitation]** Free sites are kept only for the most recent frees. For an earlier free, the diagnostic states no site. Keeping all of them would make memory grow without end as the number of frees grows.

## 13. Toolchain

### 13.1 Analysis service

- An external IDE connects to the analysis service through LSP. The command line and CI run the same analysis in headless mode. When there is a compile error, missing-value errors on scenes included, the headless analysis command returns a non-zero exit code.

### 13.2 Tools of the first version

- **Diagnostics and navigation**: the editor, LSP and the command line share live diagnostics. There are completion, hover types, go to definition, and find references that includes scenes.
- **Scenes and refactoring**: rename that includes the scene part. Scene checks for `$X`, signal connections, `PackedScene[T]`, `@parent`/`@owner` and exports that must be assigned. Quick fixes that insert inferred types, complete `match` branches, add explicit conversions and make the rewrites described throughout this document.
- **Formatting**: a single set of rules, with no options. It keeps the syntax tree, all comments and the spelling of literals unchanged, and is idempotent. A file with a syntax error is not formatted:
  - indentation is one tab per level. A comment is indented with the block it belongs to, and keeps its extra levels of indentation. A comment before the first line of a block belongs to that block. Where several blocks end at once, a comment is attributed by its original column. It is never shallower than the block of the comment that follows it;
  - a continuation line inside brackets is indented one level more than the line of the opening bracket. A closing bracket alone on a line returns to the indentation of the opening bracket's line. A `\` continuation line is indented one level more than the first line of the statement. A lambda block inside brackets is indented one level more than the line where its header ends;
  - a binary operator has one space on each side, including assignment, the `=` of a named argument, `:=` and `->`. A comma, semicolon or colon has one space after it and none before. No space goes inside brackets, after a unary operator, before call or index brackets, or around `.`. Nor is there one before `?`, inside a `$` path, or inside the braces of a single-line dictionary. A comment at the end of a line has two spaces before it. Extra spaces and trailing whitespace are removed;
  - a top-level function or class, struct and interface included, has two blank lines before and after it. Inside an inner class it has one. The comments and annotations directly above it are counted with it. The place before the first member of a block, and methods without a body, are excepted. Elsewhere, consecutive blank lines are squeezed into one. A file has no blank line at its start and a single newline at its end;
  - line breaks stay where they are. Lines are not split or joined by column width, and `;`, same-line blocks and `\` are kept. Literals, quotes, brackets, comment text, trailing commas and the file's original line endings do not change.
- **Migration**: a migration tool that works both ways between GDScript and this language.
- **Lints, debugging and testing**: lints are reported as warnings of the analysis service and suppressed with `@warning_ignore`. When a `@warning_ignore` no longer suppresses any diagnostic, it gives a hint. Connection to the engine debugger. A built-in test runner that can run headless is provided. No package manager is provided.

### 13.3 Refactoring

- A refactoring includes its half in the scenes. Changes that are mechanical and meaning-preserving are previewed, then done automatically with the refactoring, and can be undone. They include exported property keys, method names of signal connections, animation track paths and lossless value conversions. Changes that need judgment are only reported, with options. Examples are a changed node type and an existing resource that does not fit the new type.
- Renaming or moving a node in the scene editor automatically updates `$X`, `%X` and literal `get_node` paths in scripts. The update joins the same undo step, without a preview, and is listed in the Output panel afterwards. Non-literal paths, and paths that still point elsewhere in other scenes, are only reported, with a quick fix.

### 13.4 Migration

- The migration tool converts GDScript files to files of this language. The scope can be a file, a directory or the whole project. Paths that refer to a file in scenes, resources and project settings are changed along with it.
- Migration makes only the changes that the reversibility rule allows. Examples are adding `pub`, adding types, changing truthiness tests and adding `.detach()`. It introduces no feature that GDScript lacks.
- What needs judgment is left as a compile error, and a quick fix gives the options. What is not a compile error is listed in the migration report, such as by-name access with a non-literal name. Before a class is migrated, its GDScript subclasses and the GDScript files with class-level access to it are listed. An option to migrate them together is offered.
- The reverse conversion turns files of this language back into GDScript. It removes or replaces the reversible forms, and writes out the types that GDScript can express. A file that uses a feature GDScript lacks is not converted; only the places are listed.

## 14. First-version scope and open questions

### 14.2 Compatible later additions

The full designs of generics and of setters that suspend automatically before ready are in the [design notes](design-notes.md). The following features can be added compatibly after the first version:

- **Language features**:
  - user-defined generics and generic interfaces;
  - default methods of interfaces;
  - typed weak references `weak[T]`, and a quick fix in the reference cycle warning that switches to them;
  - aliases/`import`, and access to `preload` classes across classes;
  - two files each extending a class of the other;
  - a stricter `private`;
  - exported variables and `@rpc` functions implying `pub`;
  - positional construction, mutating methods, operator redefinition and indirect recursion for structs;
  - `Node` fields in exported structs and enums with payloads;
  - parameter names in function types, the direct call `f(x)` and shared captures;
  - explicit conversion of a `Callable` of unknown signature to a function type;
  - an ordinary function used implicitly where `async` is expected;
  - spreading into rest parameters at the call site, and rest parameters in function types;
  - field narrowing/conditional binding;
  - type patterns of `match`, `match` expressions and bindings in multiple patterns;
  - non-null members assigned after ready;
  - setters that suspend automatically before ready;
  - always-on assertions;
  - debug text that shows values as written in this language;
  - reentrancy annotations for coroutines, destructuring of signals with several parameters, and `Signal` with parameters.
- **Cross-language and tools**:
  - an optional runtime library with the compiler;
  - `Task` handles for GDScript coroutines;
  - views of GDScript enums/inner classes/static variables, and static method calls;
  - checking GDScript code that accesses private signals by their rewritten names;
  - advanced refactorings, deep integration with profiling, documentation generation and generation of forwarding shells.

### 14.3 Open questions

- When native code is generated: at run time, at export, or both.
- The rules for `@tool` scripts running in the editor.
