#!/usr/bin/env python

env = SConscript("thirdparty/godot-cpp/SConstruct", {"api_version": "4.7"})

env.Append(CPPPATH=["src/"])

bytecode_sources = Glob("src/bytecode/*.cpp")
compiler_sources = Glob("src/compiler/*.cpp")

# Exported games contain no compiler (architecture §1).
sources = bytecode_sources + Glob("src/runtime/*.cpp") + Glob("src/adapter/*.cpp")
if env["target"] == "editor":
    sources += compiler_sources + Glob("src/adapter/editor/*.cpp")

# The step-2 probes (architecture §5) are driven by the engine tests and stay
# out of release builds.
if env["target"] != "template_release":
    env.Append(CPPDEFINES=["PETRICHOR_PROBES"])
    sources += Glob("src/probe/*.cpp")

library = env.SharedLibrary(
    "tests/engine/project/bin/petrichor{}{}".format(env["suffix"], env["SHLIBSUFFIX"]),
    source=sources,
)

env.NoCache(library)
Default(library)

# The compiler tests are a plain program: without the godot-cpp include paths,
# they also show that the compiler does not depend on Godot (architecture §2.1).
#   scons compiler_tests && tests/compiler/bin/compiler_tests
tests_env = env.Clone()
tests_env.Replace(
    CPPPATH=["src/", "thirdparty/doctest/"],
    LIBS=[],
    LIBPATH=[],
    OBJSUFFIX=".tests" + env["OBJSUFFIX"],
)
# Engine builds disable exceptions; a failed REQUIRE then ends the run.
tests_env.Append(CPPDEFINES=["DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS"])
compiler_tests = tests_env.Program(
    "tests/compiler/bin/compiler_tests",
    source=bytecode_sources + compiler_sources + Glob("tests/compiler/*.cpp"),
)
Alias("compiler_tests", compiler_tests)
