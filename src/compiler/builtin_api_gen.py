"""Generates the tables of builtin_api.h from godot-cpp's extension_api JSON.

SConstruct runs this at build time; the output is not kept in the repository.
"""

import json


def _c_string(text):
    return '"' + text.replace("\\", "\\\\").replace('"', '\\"') + '"'


def _bool(value):
    return "true" if value else "false"


class _Pool:
    """One array of a kind of entry; entries refer into it by position."""

    def __init__(self, c_type, name):
        self.c_type = c_type
        self.name = name
        self.rows = []

    def add(self, rows):
        start = len(self.rows)
        self.rows.extend(rows)
        return start, len(rows)

    def reference(self, start, count):
        return f"{self.name} + {start}, {count}" if count else f"nullptr, 0"

    def define(self):
        if not self.rows:
            return f"static const {self.c_type} *const {self.name} = nullptr;\n"
        lines = "".join(f"\t{{ {row} }},\n" for row in self.rows)
        return f"static const {self.c_type} {self.name}[] = {{\n{lines}}};\n"


MEMBER_KINDS = {"method": "Method", "property": "Property", "property_path": "Property", "signal": "Signal"}


def _member_name(where, arguments, index, target, query):
    """A row of MEMBER_NAMES: the name's index, the object's (-1 for the
    receiver), the member's kind from the parameter's name, whether it is a
    query, the index of the value a property is given (-1 for none), and
    whether the name is a property path."""
    assert index < len(arguments), f"{where}: no argument {index}"
    named = arguments[index]
    assert named["type"] in ("StringName", "NodePath") and named["name"] in MEMBER_KINDS, f"{where}: argument {index} names no member"
    assert target == -1 or arguments[target]["type"] == "Object", f"{where}: argument {target} is not an object"
    value = next((i for i, a in enumerate(arguments) if i > index and a["name"] in ("value", "final_val")), -1)
    path = named["type"] == "NodePath"
    return f"{index}, {target}, MemberName::Kind::{MEMBER_KINDS[named['name']]}, {_bool(query)}, {value}, {_bool(path)}"


def _member_names(api, cls):
    """The names of what an engine class has, also through its bases."""
    classes = {c["name"]: c for c in api["classes"]}
    names = set()
    while cls in classes:
        c = classes[cls]
        for kind in ("methods", "properties", "signals", "constants", "enums"):
            names.update(m["name"] for m in c.get(kind, []))
        cls = c.get("inherits")
    return names


def _binding_metadata(api, metadata_path):
    """The engine's RequiredResult and RequiredParam metadata, merged with the
    audit in binding_metadata.txt (spec §10.2, §10.3). Returns sorted lists of
    what is never null, "Class.method", "Class.property" and
    "Class.method:index", of the parameters that filter by class,
    "Class.method:index", of those that name a member, ("Class.method",
    row), of the methods with a try_ version, ("Class.method", getter), and
    of the signal parameters that may be null, "Class.signal:index"."""
    methods = {(c["name"], m["name"]): m for c in api["classes"] for m in c.get("methods", [])}
    signals = {(c["name"], s["name"]): s for c in api["classes"] for s in c.get("signals", [])}
    objects = {c["name"] for c in api["classes"]}
    returns = {f"{c}.{m}" for (c, m), info in methods.items() if info.get("return_value", {}).get("meta") == "required"}
    params = {
        f"{c}.{m}:{i}"
        for (c, m), info in methods.items()
        for i, a in enumerate(info.get("arguments", []))
        if a.get("meta") == "required"
    }
    filters = set()
    names = {}
    getters = {}
    nullable = set()
    with open(metadata_path, encoding="utf-8") as file:
        for number, line in enumerate(file, 1):
            words = line.split("#", 1)[0].split()
            if not words:
                continue
            where = f"{metadata_path}:{number}"
            assert len(words) >= 2, f"{where}: expected a kind and Class.method"
            cls, _, method = words[1].partition(".")
            if words[0] == "emits-null":
                assert len(words) == 4 and words[2].isdigit(), f"{where}: expected 'emits-null Class.signal index path:line'"
                arguments = signals.get((cls, method), {}).get("arguments", [])
                assert int(words[2]) < len(arguments) and arguments[int(words[2])]["type"] in objects, f"{where}: {words[1]} has no object argument {words[2]}"
                nullable.add(f"{words[1]}:{words[2]}")
                continue
            assert (cls, method) in methods, f"{where}: the engine has no method {words[1]}"
            if words[0] == "returns-non-null":
                assert len(words) == 3, f"{where}: expected 'returns-non-null Class.method path:line'"
                returns.add(words[1])
            elif words[0] == "filters-by-class":
                assert len(words) == 4 and words[2].isdigit(), f"{where}: expected 'filters-by-class Class.method index path:line'"
                arguments = methods[(cls, method)].get("arguments", [])
                index = int(words[2])
                assert index < len(arguments) and arguments[index]["type"] in ("String", "StringName"), f"{where}: argument {index} is not a class name"
                assert methods[(cls, method)].get("return_value", {}).get("type", "").startswith("typedarray::"), f"{where}: {words[1]} does not return a typed array"
                filters.add(f"{words[1]}:{index}")
            elif words[0] in ("names-member", "queries-member"):
                assert len(words) == 5 and words[2].isdigit() and (words[3] == "-" or words[3].isdigit()), f"{where}: expected '{words[0]} Class.method index object path:line'"
                assert words[1] not in names, f"{where}: {words[1]} is listed twice"
                target = -1 if words[3] == "-" else int(words[3])
                names[words[1]] = _member_name(where, methods[(cls, method)].get("arguments", []), int(words[2]), target, words[0] == "queries-member")
            elif words[0] == "reports-error":
                assert len(words) == 4, f"{where}: expected 'reports-error Class.method getter path:line'"
                getter = methods.get((cls, words[2]), {})
                assert methods[(cls, method)].get("is_static"), f"{where}: {words[1]} is not static"
                assert getter.get("is_static") and not getter.get("arguments") and getter.get("return_value", {}).get("type") == "enum::Error", f"{where}: {words[2]} of {cls} is no static getter of an Error"
                # The try_ version's name is this language's (spec §10.3).
                assert f"try_{method}" not in _member_names(api, cls), f"{where}: the engine now has {cls}.try_{method}"
                getters[words[1]] = words[2]
            else:
                assert False, f"{where}: unknown kind {words[0]}"
    # A property reads through its getter.
    properties = {
        f"{c['name']}.{p['name']}"
        for c in api["classes"]
        for p in c.get("properties", [])
        if p.get("getter") and f"{c['name']}.{p['getter']}" in returns
    }
    return sorted(returns), sorted(properties), sorted(params), sorted(filters), sorted(names.items()), sorted(getters.items()), sorted(nullable)


# Integer widths the engine truncates or wraps a value to (spec §10.6). A
# uint64 holds the same bits as an int, so it passes them unchecked.
NARROW_INTS = ("int8", "int16", "int32", "uint8", "uint16", "uint32", "char32")


def _int_ranges(api):
    """The engine's integer parameters narrower than int64, as
    sorted (key, width) pairs: "Class.method:index", and "Class.property" for
    the value a property's setter takes, its last argument."""
    params = {}
    properties = {}
    for c in api["classes"]:
        methods = {m["name"]: m for m in c.get("methods", [])}
        for m in methods.values():
            for i, a in enumerate(m.get("arguments", [])):
                if a["type"] == "int" and a.get("meta") in NARROW_INTS:
                    params[f"{c['name']}.{m['name']}:{i}"] = a["meta"]
        for p in c.get("properties", []):
            arguments = methods.get(p.get("setter", ""), {}).get("arguments", [])
            if p["type"] == "int" and arguments and arguments[-1]["type"] == "int" and arguments[-1].get("meta") in NARROW_INTS:
                properties[f"{c['name']}.{p['name']}"] = arguments[-1]["meta"]
    return sorted(params.items()), sorted(properties.items())


def _property_indices(api):
    """The index that a property of a family, such as a BaseMaterial3D texture,
    passes its getter and setter first, as sorted ("Class.property", index)
    pairs; ClassDB does not tell it to scripts."""
    return sorted((f"{c['name']}.{p['name']}", p["index"]) for c in api["classes"] for p in c.get("properties", []) if "index" in p)


def _enums(api):
    """The engine's global enums, such as Key and Variant.Type, and those of its
    built-in types, such as Vector3.Axis, as sorted (name, is_bitfield, values)."""
    enums = [(e["name"], e.get("is_bitfield", False), [(v["name"], v["value"]) for v in e["values"]]) for e in api["global_enums"]]
    for c in api["builtin_classes"]:
        for e in c.get("enums", []):
            enums.append((f"{c['name']}.{e['name']}", e.get("is_bitfield", False), [(v["name"], v["value"]) for v in e["values"]]))
    return sorted(enums)


def _global_constants(api, enums):
    """The values of the engine's global enums, such as KEY_ESCAPE, and its
    global constants, such as INT32_MAX, as sorted (name, value, index of the
    enum or -1) triples."""
    index = {name: i for i, (name, _, _) in enumerate(enums)}
    rows = [(v["name"], v["value"], index[e["name"]]) for e in api["global_enums"] for v in e["values"]]
    rows += [(c["name"], c["value"], -1) for c in api.get("global_constants", [])]
    assert len({name for name, _, _ in rows}) == len(rows), "global constant names are unique"
    return sorted(rows)


def _int64(value):
    return "INT64_MIN" if value == -(2**63) else f"INT64_C({value})"


def generate(json_path, metadata_path, out_path):
    with open(json_path, encoding="utf-8") as file:
        api = json.load(file)
    metadata = _binding_metadata(api, metadata_path)

    arguments = _Pool("Argument", "ARGUMENTS")
    members = _Pool("Member", "MEMBERS")
    constants = _Pool("Constant", "CONSTANTS")
    operators = _Pool("Operator", "OPERATORS")
    methods = _Pool("Method", "METHODS")
    constructors = _Pool("Constructor", "CONSTRUCTORS")

    def argument_rows(items):
        return [
            f"{_c_string(a['name'])}, {_c_string(a['type'])}, {_bool('default_value' in a)}, {_c_string(a.get('default_value', ''))}"
            for a in items or []
        ]

    def method_row(m):
        start, count = arguments.add(argument_rows(m.get("arguments")))
        return ", ".join([
            _c_string(m["name"]),
            _c_string(m.get("return_type", "")),
            f"{m.get('hash', 0)}u",
            arguments.reference(start, count),
            _bool(m.get("is_vararg", False)),
            _bool(m.get("is_static", False)),
            _bool(m.get("is_const", False)),
        ])

    classes = []
    for cls in api["builtin_classes"]:
        member_ref = members.add([f"{_c_string(m['name'])}, {_c_string(m['type'])}" for m in cls.get("members", [])])
        constant_ref = constants.add([f"{_c_string(c['name'])}, {_c_string(c['type'])}" for c in cls.get("constants", [])])
        operator_ref = operators.add([
            f"{_c_string(o['name'])}, {_c_string(o.get('right_type', ''))}, {_c_string(o['return_type'])}"
            for o in cls.get("operators", [])
        ])
        method_ref = methods.add([method_row(m) for m in cls.get("methods", [])])
        constructor_rows = []
        for constructor in sorted(cls.get("constructors", []), key=lambda c: c["index"]):
            assert constructor["index"] == len(constructor_rows), "constructors are numbered in order"
            start, count = arguments.add(argument_rows(constructor.get("arguments")))
            constructor_rows.append(arguments.reference(start, count))
        constructor_ref = constructors.add(constructor_rows)
        classes.append(", ".join([
            _c_string(cls["name"]),
            members.reference(*member_ref),
            constants.reference(*constant_ref),
            operators.reference(*operator_ref),
            methods.reference(*method_ref),
            constructors.reference(*constructor_ref),
        ]))

    utilities = []
    for u in api["utility_functions"]:
        start, count = arguments.add(argument_rows(u.get("arguments")))
        utilities.append(", ".join([
            _c_string(u["name"]),
            _c_string(u.get("return_type", "")),
            f"{u['hash']}u",
            arguments.reference(start, count),
            _bool(u.get("is_vararg", False)),
            _bool(u.get("category") == "math"),
        ]))

    header = api["header"]
    with open(out_path, "w", encoding="utf-8", newline="\n") as out:
        out.write("// Generated by src/compiler/builtin_api_gen.py from ")
        out.write(f"{header['version_full_name']} ({header['precision']} precision). Do not edit.\n\n")
        out.write('#include "compiler/builtin_api.h"\n\n')
        out.write("namespace petrichor::compiler::builtin {\n\n")
        for pool in (arguments, members, constants, operators, methods, constructors):
            out.write(pool.define())
            out.write("\n")
        out.write("extern const Class CLASSES[];\nextern const size_t CLASS_COUNT;\n")
        out.write("extern const Utility UTILITIES[];\nextern const size_t UTILITY_COUNT;\n\n")
        out.write("const Class CLASSES[] = {\n" + "".join(f"\t{{ {row} }},\n" for row in classes) + "};\n")
        out.write(f"const size_t CLASS_COUNT = {len(classes)};\n\n")
        out.write("const Utility UTILITIES[] = {\n" + "".join(f"\t{{ {row} }},\n" for row in utilities) + "};\n")
        out.write(f"const size_t UTILITY_COUNT = {len(utilities)};\n\n")
        for name, rows in zip(("NON_NULL_RETURNS", "NON_NULL_PROPERTIES", "NON_NULL_PARAMS", "CLASS_FILTERS", "NULLABLE_SIGNAL_PARAMS"), (*metadata[:4], metadata[6])):
            out.write(f"extern const char *const {name}[];\nextern const size_t {name}_COUNT;\n")
            out.write(f"const char *const {name}[] = {{\n" + "".join(f"\t{_c_string(row)},\n" for row in rows) + "};\n")
            out.write(f"const size_t {name}_COUNT = {len(rows)};\n\n")
        rows = metadata[4]
        out.write("extern const MemberNameEntry MEMBER_NAMES[];\nextern const size_t MEMBER_NAMES_COUNT;\n")
        out.write("const MemberNameEntry MEMBER_NAMES[] = {\n" + "".join(f"\t{{ {_c_string(key)}, {{ {row} }} }},\n" for key, row in rows) + "};\n")
        out.write(f"const size_t MEMBER_NAMES_COUNT = {len(rows)};\n\n")
        for name, rows in zip(("INT_PARAMS", "INT_PROPERTIES", "ERROR_GETTERS"), (*_int_ranges(api), metadata[5])):
            out.write(f"extern const Keyed {name}[];\nextern const size_t {name}_COUNT;\n")
            out.write(f"const Keyed {name}[] = {{\n" + "".join(f"\t{{ {_c_string(key)}, {_c_string(value)} }},\n" for key, value in rows) + "};\n")
            out.write(f"const size_t {name}_COUNT = {len(rows)};\n\n")
        rows = _property_indices(api)
        out.write("extern const KeyedIndex PROPERTY_INDICES[];\nextern const size_t PROPERTY_INDICES_COUNT;\n")
        out.write("const KeyedIndex PROPERTY_INDICES[] = {\n" + "".join(f"\t{{ {_c_string(key)}, {index} }},\n" for key, index in rows) + "};\n")
        out.write(f"const size_t PROPERTY_INDICES_COUNT = {len(rows)};\n\n")
        enums = _enums(api)
        values, enum_rows = [], []
        for name, is_bitfield, entries in enums:
            enum_rows.append(f"{_c_string(name)}, {_bool(is_bitfield)}, ENUM_VALUES + {len(values)}, {len(entries)}")
            values += entries
        out.write("extern const EnumValue ENUM_VALUES[];\nextern const Enum ENUMS[];\nextern const size_t ENUM_COUNT;\n")
        out.write("const EnumValue ENUM_VALUES[] = {\n" + "".join(f"\t{{ {_c_string(name)}, {_int64(value)} }},\n" for name, value in values) + "};\n")
        out.write("const Enum ENUMS[] = {\n" + "".join(f"\t{{ {row} }},\n" for row in enum_rows) + "};\n")
        out.write(f"const size_t ENUM_COUNT = {len(enum_rows)};\n\n")
        rows = _global_constants(api, enums)
        out.write("extern const GlobalConstant GLOBAL_CONSTANTS[];\nextern const size_t GLOBAL_CONSTANTS_COUNT;\n")
        out.write("const GlobalConstant GLOBAL_CONSTANTS[] = {\n" + "".join(f"\t{{ {_c_string(name)}, {_int64(value)}, {enum} }},\n" for name, value, enum in rows) + "};\n")
        out.write(f"const size_t GLOBAL_CONSTANTS_COUNT = {len(rows)};\n\n")
        out.write("} // namespace petrichor::compiler::builtin\n")


def generate_value_types(api_path, out_path):
    with open(api_path, encoding="utf-8") as file:
        api = json.load(file)
    sizes = next(entry["sizes"] for entry in api["builtin_class_sizes"] if entry["build_configuration"] == "float_64")
    packed = {
        c["name"]: {"int": "Int", "float": "Float"}.get(c["indexing_return_type"], c["indexing_return_type"])
        for c in api["builtin_classes"]
        if c["name"].startswith("Packed") and "indexing_return_type" in c
    }
    with open(out_path, "w", encoding="utf-8", newline="\n") as out:
        out.write("#pragma once\n\n// Generated by src/compiler/builtin_api_gen.py.\n\n")
        out.write("namespace petrichor::bytecode {\n\nstruct TypeInfo {\n\tconst char *name;\n\tuint32_t size;\n\tValueType packed_element;\n};\n\nconst TypeInfo TYPES[] = {\n")
        for s in sizes:
            raw, size = s["name"], s["size"]
            name = "void" if raw == "Nil" else raw
            size = 8 if raw == "bool" else (16 if raw == "Object" else size)
            out.write(f'\t{{ {_c_string(name)}, {size}, ValueType::{packed.get(raw, "Void")} }},\n')
        out.write('\t{ "Task", 8, ValueType::Void },\n};\n\n} // namespace petrichor::bytecode\n')
