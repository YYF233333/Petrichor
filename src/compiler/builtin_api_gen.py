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


def _binding_metadata(api, metadata_path):
    """The engine's RequiredResult and RequiredParam metadata, merged with the
    audit in binding_metadata.txt (spec §10.2, §10.3). Returns sorted lists of
    what is never null, "Class.method", "Class.property" and
    "Class.method:index", and of the parameters that filter by class,
    "Class.method:index"."""
    methods = {(c["name"], m["name"]): m for c in api["classes"] for m in c.get("methods", [])}
    returns = {f"{c}.{m}" for (c, m), info in methods.items() if info.get("return_value", {}).get("meta") == "required"}
    params = {
        f"{c}.{m}:{i}"
        for (c, m), info in methods.items()
        for i, a in enumerate(info.get("arguments", []))
        if a.get("meta") == "required"
    }
    filters = set()
    with open(metadata_path, encoding="utf-8") as file:
        for number, line in enumerate(file, 1):
            words = line.split("#", 1)[0].split()
            if not words:
                continue
            where = f"{metadata_path}:{number}"
            assert len(words) >= 2, f"{where}: expected a kind and Class.method"
            cls, _, method = words[1].partition(".")
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
            else:
                assert False, f"{where}: unknown kind {words[0]}"
    # A property reads through its getter.
    properties = {
        f"{c['name']}.{p['name']}"
        for c in api["classes"]
        for p in c.get("properties", [])
        if p.get("getter") and f"{c['name']}.{p['getter']}" in returns
    }
    return sorted(returns), sorted(properties), sorted(params), sorted(filters)


# Integer widths the engine truncates or wraps a value to (spec §10.6).
NARROW_INTS = ("int8", "int16", "int32", "uint8", "uint16", "uint32", "uint64", "char32")


def _int_ranges(api):
    """The engine's integer parameters narrower than int64 or unsigned, as
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
        for name, rows in zip(("NON_NULL_RETURNS", "NON_NULL_PROPERTIES", "NON_NULL_PARAMS", "CLASS_FILTERS"), metadata):
            out.write(f"extern const char *const {name}[];\nextern const size_t {name}_COUNT;\n")
            out.write(f"const char *const {name}[] = {{\n" + "".join(f"\t{_c_string(row)},\n" for row in rows) + "};\n")
            out.write(f"const size_t {name}_COUNT = {len(rows)};\n\n")
        for name, rows in zip(("INT_PARAMS", "INT_PROPERTIES"), _int_ranges(api)):
            out.write(f"extern const Keyed {name}[];\nextern const size_t {name}_COUNT;\n")
            out.write(f"const Keyed {name}[] = {{\n" + "".join(f"\t{{ {_c_string(key)}, {_c_string(width)} }},\n" for key, width in rows) + "};\n")
            out.write(f"const size_t {name}_COUNT = {len(rows)};\n\n")
        out.write("} // namespace petrichor::compiler::builtin\n")
