set_xmakever("3.0.6")
set_project("strnghsh")
set_version("0.1.0")
set_languages("c++23")
add_rules("mode.debug", "mode.release")
set_policy("build.c++.modules.std", true)

target("strnghsh")
    set_kind("static")
    add_files("src/strnghsh.cppm", {public = true})
    if is_plat("linux", "bsd") then
        add_syslinks("pthread", {public = true})
    end

target("strnghsh_tests")
    set_kind("binary")
    set_default(false)
    add_deps("strnghsh")
    add_files("tests/strnghsh.cpp")
    add_tests("default")
