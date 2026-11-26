-- RadialCommandMod (LeviLamina client native mod) + standalone core tests.
--
-- Targets:
--  * radial_core:      pure C++20 logic (i18n/UTF-8, config/JSONC, server matcher,
--                      radial controller, scanline wheel geometry, editor state model).
--                      No LeviLamina dependency.
--  * radial_tests:     standalone unit test suite (`xmake run radial_tests`).
--  * RadialCommandMod: LeviLamina 26.60.8 client mod DLL.
set_xmakever("3.0.0")
add_rules("mode.release", "mode.debug")

add_repositories("levimc-repo https://github.com/LiteLDev/xmake-repo.git")

option("with_mod")
    set_default(true)
    set_showmenu(true)
    set_description("Build the LeviLamina client mod DLL")
option_end()

if has_config("with_mod") then
    add_requires("levilamina 26.60.8", {configs = {target_type = "client"}})
    add_requires("levibuildscript")
end

-- Core: NO LeviLamina dependency (unit-testable standalone).
target("radial_core")
    set_kind("static")
    set_languages("c++20")
    add_files("src/core/*.cpp")
    add_includedirs("src", {public = true})
    if is_plat("windows") then
        set_runtimes("MD")
        add_defines("NOMINMAX", "UNICODE", "_HAS_CXX23=1")
        add_cxflags("/utf-8")
    end

-- Standalone unit tests (no LeviLamina, no game).
target("radial_tests")
    set_kind("binary")
    set_languages("c++20")
    add_undefines("NDEBUG")
    set_default(true)
    add_files("tests/test_radial.cpp")
    add_deps("radial_core")
    if is_plat("windows") then
        set_runtimes("MD")
        add_defines("NOMINMAX", "UNICODE", "_HAS_CXX23=1")
        add_cxflags("/utf-8")
    end

-- LeviLamina client mod DLL.
if has_config("with_mod") then
target("RadialCommandMod")
    set_kind("shared")
    set_languages("c++20")
    add_rules("@levibuildscript/linkrule")
    add_rules("@levibuildscript/modpacker", {
        modName = "RadialCommandMod",
        modFile = "RadialCommandMod.dll",
        modVersion = "1.0.1",
        modPlatform = "client"
    })
    add_files("src/core/*.cpp", "src/mod/*.cpp")
    add_includedirs("src")
    add_packages("levilamina")
    set_symbols("debug")
    if is_plat("windows") then
        set_toolchains("clang-cl")
        set_runtimes("MD")
        add_syslinks("user32")
        add_defines("NOMINMAX", "UNICODE", "_HAS_CXX23=1", "LL_PLAT_C")
        add_cxflags("/utf-8")
        -- Strip local absolute PDB path (Windows username) from release DLLs.
        add_shflags("/PDBALTPATH:%_PDB%", {force = true})
    end
end
