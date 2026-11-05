#!/usr/bin/env python

import hashlib
import os
import sys

# Finding MSVC runs vcvarsall.bat, nearly a second on every build; SCons can
# keep what it found (in ~/.scons_msvc_cache.json).
os.environ.setdefault("SCONS_CACHE_MSVC_CONFIG", "1")

# The editor library is the one the tests and benchmarks load.
ARGUMENTS.setdefault("target", "editor")


def configure_godot_cpp():
    godot_cpp = Dir("thirdparty/godot-cpp")
    sys.path.append(godot_cpp.abspath)
    env = Environment(tools=["default"], PLATFORM="", api_version="4.7", binding_hooks=None)
    env.PrependENVPath("PATH", os.getenv("PATH"))
    customs = ["custom.py"]
    profile = ARGUMENTS.get("profile", "")
    if profile:
        if os.path.isfile(profile):
            customs.append(profile)
        elif os.path.isfile(profile + ".py"):
            customs.append(profile + ".py")
    env.fs.chdir(godot_cpp, change_os_dir=True)
    try:
        opts = Variables(customs, ARGUMENTS)
        cpp_tool = Tool("godotcpp", toolpath=[godot_cpp.Dir("tools").abspath])
        cpp_tool.options(opts, env)
        opts.Update(env)
        Help(opts.GenerateHelpText(env))
        unknown = opts.UnknownVariables()
        if unknown:
            print("WARNING: Unknown SCons variables were passed and will be ignored:")
            for key, value in unknown.items():
                print("    " + key + "=" + value)
        scons_cache_path = os.environ.get("SCONS_CACHE")
        if scons_cache_path is not None:
            CacheDir(scons_cache_path)
        cpp_tool.generate(env)
    finally:
        env.fs.chdir(Dir("#"), change_os_dir=True)
    return env


# Walking godot-cpp's generated bindings parses extension_api-4-7.json twice
# and registers ~7,000 file nodes on every build. Once libgodot-cpp is built,
# skip that registration unless compiler flags or godot-cpp's sources changed.
def godot_cpp_stamp(env):
    digest = hashlib.sha256()
    for key in (
        "CC",
        "CXX",
        "AR",
        "CCFLAGS",
        "CXXFLAGS",
        "CPPDEFINES",
        "ARFLAGS",
        "api_version",
        "custom_api_file",
        "gdextension_dir",
        "build_profile",
        "generate_template_get_node",
        "precision",
        "arch",
        "platform",
    ):
        digest.update("{}={!r}\0".format(key, env.get(key)).encode())
    root = Dir("thirdparty/godot-cpp").abspath
    source_exts = {".h", ".hpp", ".cpp", ".inc", ".py", ".json"}
    entries = []
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = sorted(d for d in dirnames if d not in ("gen", "bin", ".git", "__pycache__", "test"))
        for name in sorted(filenames):
            if name == "SConstruct" or os.path.splitext(name)[1] in source_exts:
                full = os.path.join(dirpath, name)
                stat = os.stat(full)
                entries.append("{}:{}:{}\n".format(os.path.relpath(full, root), stat.st_mtime_ns, stat.st_size))
    digest.update("".join(entries).encode())
    return digest.hexdigest()


def write_godot_cpp_stamp(target, source, env):
    os.makedirs(os.path.dirname(str(target[0])), exist_ok=True)
    with open(str(target[0]), "w", newline="\n") as file:
        file.write(source[1].read() + "\n")


godot_cpp_env = configure_godot_cpp()
godot_cpp_env.Decider("content-timestamp")
SetOption("implicit_cache", 1)

only_compiler_tests = list(COMMAND_LINE_TARGETS) == ["compiler_tests"] and not GetOption("clean") and not GetOption("help")
godot_cpp_stamp_node = None
if not only_compiler_tests:
    godot_cpp_lib_path = "thirdparty/godot-cpp/bin/libgodot-cpp{}{}".format(godot_cpp_env["suffix"], godot_cpp_env["LIBSUFFIX"])
    stamp_path = "build/godot_cpp{}.stamp".format(godot_cpp_env["suffix"])
    stamp_value = godot_cpp_stamp(godot_cpp_env)
    can_skip = (
        not GetOption("clean")
        and not GetOption("help")
        and not godot_cpp_env["generate_bindings"]
        and not godot_cpp_env["compiledb"]
        and "compiledb" not in COMMAND_LINE_TARGETS
        and godot_cpp_env["build_library"]
        and os.path.isfile(godot_cpp_lib_path)
        and os.path.isfile("thirdparty/godot-cpp/gen/include/gdextension_interface.h")
        and os.path.isfile(stamp_path)
    )
    if can_skip:
        with open(stamp_path, encoding="utf-8") as stamp_file:
            can_skip = stamp_file.read().strip() == stamp_value
    if can_skip:
        godot_cpp_env.AppendUnique(
            CPPPATH=[Dir("thirdparty/godot-cpp/include"), Dir("thirdparty/godot-cpp/gen/include")],
            LIBS=[File(godot_cpp_lib_path)],
        )
    else:
        godot_cpp_env.fs.chdir(Dir("thirdparty/godot-cpp"), change_os_dir=True)
        try:
            godot_cpp_lib = godot_cpp_env.GodotCPP()
        finally:
            godot_cpp_env.fs.chdir(Dir("#"), change_os_dir=True)
        if godot_cpp_lib is not None:
            godot_cpp_stamp_node = godot_cpp_env.Command(
                stamp_path,
                [godot_cpp_lib, Value(stamp_value)],
                Action(write_godot_cpp_stamp, None),
            )

# godot-cpp declared its own objects with that environment, so settings of this
# project go into copies, or every change to them would rebuild godot-cpp.
env = godot_cpp_env.Clone()
env.Append(CPPPATH=["src/", "gen/"])

# The compiler's tables of built-in types and utility functions come from the
# extension API that godot-cpp is built against (architecture §2.1).
sys.path.insert(0, Dir("src/compiler").abspath)
import builtin_api_gen


def generate_builtin_api(target, source, env):
    builtin_api_gen.generate(str(source[0]), str(source[1]), str(target[0]))


# Generated where it is compiled, so that the object depends on the generator's
# inputs: a source the variant directory below reads from gen/ is never rebuilt.
env.Command(
    "build/gen/builtin_api.gen.cpp",
    ["thirdparty/godot-cpp/gdextension/extension_api-4-7.json", "src/compiler/binding_metadata.txt", "src/compiler/builtin_api_gen.py"],
    Action(generate_builtin_api, "Generating $TARGET"),
)


# The compiled format has no version number: a file is read only by builds
# whose src/bytecode/ is the same, which this hash identifies (architecture
# §2.4). Line endings are normalized, so that checkouts on different systems
# agree. The hash is taken while reading this file, since the sources include
# the header it goes into.
def format_id():
    digest = hashlib.sha256()
    for node in sorted(Glob("src/bytecode/*.h") + Glob("src/bytecode/*.cpp"), key=lambda node: node.name):
        digest.update(node.name.encode() + b"\0")
        digest.update(node.get_contents().replace(b"\r\n", b"\n") + b"\0")
    return digest.hexdigest()[:16]


def generate_format_id(target, source, env):
    with open(str(target[0]), "w", newline="\n") as file:
        file.write("#pragma once\n\n")
        file.write("// Generated by SConstruct from src/bytecode/.\n\n")
        file.write("#include <cstdint>\n\n")
        file.write("namespace petrichor::bytecode {\n\n")
        file.write("constexpr uint64_t FORMAT_ID = 0x{}ull;\n\n".format(source[0].read()))
        file.write("} // namespace petrichor::bytecode\n")


env.Command("gen/format_id.gen.h", Value(format_id()), Action(generate_format_id, "Generating $TARGET"))

# Objects and the static libraries go to build/, not next to the sources.
VariantDir("build/src", "src", duplicate=0)
VariantDir("build/gen", "gen", duplicate=0)
VariantDir("build/tests", "tests", duplicate=0)

# Neither bytecode, compiler nor runtime checks TOOLS_ENABLED or
# PETRICHOR_PROBES: editor, template_debug and compiler_tests share their
# objects, while template_release keeps its own non-DEBUG_ENABLED suffix.
shared_suffix = env["suffix"].replace(".editor.", ".debug.").replace(".template_debug.", ".debug.")
shared_env = env.Clone()
shared_env["CPPDEFINES"] = [d for d in shared_env["CPPDEFINES"] if d != "TOOLS_ENABLED"]
shared_env["OBJSUFFIX"] = shared_env["OBJSUFFIX"].replace(env["suffix"], shared_suffix)
if shared_env["SHOBJSUFFIX"] != "$OBJSUFFIX":
    shared_env["SHOBJSUFFIX"] = shared_suffix + shared_env["SHOBJSUFFIX"]

# The bytecode and the compiler do not depend on Godot (architecture §2.1):
# they are built without the godot-cpp include paths, once, for the library and
# the compiler tests.
core_env = shared_env.Clone()
core_env.Replace(CPPPATH=["src/", "gen/"])
bytecode_library = core_env.StaticLibrary("build/petrichor_bytecode" + shared_suffix + core_env["LIBSUFFIX"], Glob("build/src/bytecode/*.cpp"))
compiler_library = core_env.StaticLibrary("build/petrichor_compiler" + shared_suffix + core_env["LIBSUFFIX"], Glob("build/src/compiler/*.cpp") + ["build/gen/builtin_api.gen.cpp"])

if not only_compiler_tests:
    # Exported games contain no compiler (architecture §1).
    runtime_objects = [shared_env.SharedObject(src) for src in Glob("build/src/runtime/*.cpp")]
    sources = runtime_objects + Glob("build/src/adapter/*.cpp")
    libraries = [bytecode_library]
    if env["target"] == "editor":
        sources += Glob("build/src/adapter/editor/*.cpp")
        libraries.insert(0, compiler_library)

    # The step-2 probes (architecture §5) are driven by the engine tests, which load
    # only the editor library.
    if env["target"] == "editor":
        env.Append(CPPDEFINES=["PETRICHOR_PROBES"])
        sources += Glob("build/src/probe/*.cpp")

    env.Prepend(LIBS=libraries)
    library = env.SharedLibrary(
        "tests/engine/project/bin/petrichor{}{}".format(env["suffix"], env["SHLIBSUFFIX"]),
        source=sources,
    )
    # Not Depends: the skip path has no stamp node, and a change in the number
    # of dependencies would relink the library.
    if godot_cpp_stamp_node is not None:
        Requires(library, godot_cpp_stamp_node)

    env.NoCache(library)
    Default(library)

#   scons compiler_tests && tests/compiler/bin/compiler_tests
tests_env = core_env.Clone()
tests_env.Append(CPPPATH=["thirdparty/doctest/"])
tests_env.Replace(LIBS=[compiler_library, bytecode_library], LIBPATH=[])
# Engine builds disable exceptions; a failed REQUIRE then ends the run.
tests_env.Append(CPPDEFINES=["DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS"])
compiler_tests = tests_env.Program("tests/compiler/bin/compiler_tests", Glob("build/tests/compiler/*.cpp"))
Alias("compiler_tests", compiler_tests)
