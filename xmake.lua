-- set minimum xmake version
set_xmakever("2.7.8")

-- set project
set_project("template-commonlibsse-ng")
set_version("0.0.0")
set_license("MIT")
set_languages("c++20")
set_optimize("faster")
set_warnings("allextra", "error")

-- set allowed
set_allowedarchs("windows|x64")
set_allowedmodes("debug", "releasedbg")

-- set defaults
set_defaultarchs("windows|x64")
set_defaultmode("releasedbg")

-- add rules
add_rules("mode.debug", "mode.releasedbg")
add_rules("plugin.vsxmake.autoupdate")

-- set policies
set_policy("package.requires_lock", true)

-- require packages
add_requires("commonlibsse-ng", { configs = { skyrim_vr = false } })

-- targets
target("direction-plugin")
    -- add packages to target
    add_packages("fmt", "spdlog", "commonlibsse-ng", "imgui")
    
    -- add commonlibsse-ng plugin
    add_rules("@commonlibsse-ng/plugin", {
        name = "direction-plugin",
        author = "aaaa",
        description = "SKSE64 plugin template using CommonLibSSE-NG"
    })

    -- add src files
    add_files("src/plugin/**.cpp")
    add_headerfiles("src/plugin/**.h")
    add_includedirs("src/plugin")
    set_pcxxheader("src/plugin/pch.h")