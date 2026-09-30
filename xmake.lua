-- include subprojects
includes("lib/commonlibsse-ng")

-- set project constants
set_project("direction-plugin")
set_version("0.0.0")
set_license("GPL-3.0")
set_languages("c++23")
set_warnings("allextra")
set_optimize("faster")

-- add common rules
add_rules("mode.debug", "mode.releasedbg")
add_rules("plugin.vsxmake.autoupdate")

-- imgui static library (vendored via submodule at lib/imgui)
target("imgui")
    set_kind("static")
    add_files(
        "lib/imgui/imgui.cpp",
        "lib/imgui/imgui_demo.cpp",
        "lib/imgui/imgui_draw.cpp",
        "lib/imgui/imgui_tables.cpp",
        "lib/imgui/imgui_widgets.cpp"
    )
    add_includedirs("lib/imgui", { public = true })

-- minhook static library (vendored via submodule at lib/minhook)
-- Provides function-entry detours with proper prologue analysis (HDE64).
-- Used for hooks where SKSE's write_call/write_branch isn't enough — e.g.
-- intercepting every call to a non-virtual engine function.
target("minhook")
    set_kind("static")
    add_files(
        "lib/minhook/src/buffer.c",
        "lib/minhook/src/hook.c",
        "lib/minhook/src/trampoline.c",
        "lib/minhook/src/hde/hde64.c"
    )
    add_includedirs("lib/minhook/include", { public = true })

-- define plugin target
target("direction-plugin")
    add_deps("imgui", "minhook")

    add_rules("commonlibsse-ng.plugin", {
        name = "direction-plugin",
        author = "ben",
        description = "Skyrim combat mod"
    })

    add_defines("NOMINMAX", "WIN32_LEAN_AND_MEAN")

    -- add src files
    add_files("src/**.cpp")
    add_headerfiles("src/**.h")
    add_includedirs("src")
    set_pcxxheader("src/pch.h")
