#!/usr/bin/env python

env = SConscript("thirdparty/godot-cpp/SConstruct", {"api_version": "4.7"})

env.Append(CPPPATH=["src/"])

sources = Glob("src/runtime/*.cpp") + Glob("src/adapter/*.cpp")
if env["target"] == "editor":
    sources += Glob("src/adapter/editor/*.cpp")

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
