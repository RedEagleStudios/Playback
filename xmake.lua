add_rules("mode.debug", "mode.release")

-- dev: the release build without optimisation, for edit-build-test loops (xmake f -m dev). It keeps release's defines
-- and runtime, so the mod behaves the same in game; only code generation differs. Release and CI builds are unchanged.
if is_mode("dev") then
    set_optimize("none")
    add_defines("NDEBUG")
end

add_repositories("levimc-repo https://github.com/LiteLDev/xmake-repo.git")

package("playback-ffmpeg")
    set_kind("binary")
    set_base("ffmpeg")
    add_urls("https://ffmpeg.org/releases/ffmpeg-$(version).tar.bz2")
    add_versions("7.1", "fd59e6160476095082e94150ada5a6032d7dcc282fe38ce682a00c18e7820528")
    on_load(function (package)
        package:add("deps", "nasm")
        if package:config("libx264") then
            package:add("deps", "x264")
        end
        package:add("deps", "msys2", {configs = {msystem = "MINGW64", base_devel = true}})
    end)
    on_install("@windows", function (package)
        local msys2      = assert(package:dep("msys2"), "MSYS2 dependency was not resolved")
        local msys2_base = assert(msys2:dep("msys2-base"), "MSYS2 base dependency was not resolved")
        local bash        = path.join(msys2_base:installdir("usr", "bin"), "bash.exe")
        -- xmake-repo's ffmpeg install runs uchardet from configure. MSYS2 no longer packages it for MINGW64, so install
        -- the UCRT64 build and put a wrapper on the PATH; the executable finds its DLLs in its own directory.
        os.vrunv(bash, {
            "-leo",
            "pipefail",
            "-c",
            "pacman --noconfirm -S --needed --overwrite '*' make mingw-w64-ucrt-x86_64-uchardet " ..
                "mingw-w64-x86_64-gcc mingw-w64-x86_64-iconv mingw-w64-x86_64-pkgconf && " ..
                "printf '#!/bin/sh\\nexec /ucrt64/bin/uchardet \"$@\"\\n' > /usr/bin/uchardet && chmod +x /usr/bin/uchardet"
        })
        package:base():script("install")(package)
    end)
    on_test(function (package)
        os.vrun("ffmpeg -version")
    end)
package_end()

option("target_type")
    set_default("client")
    set_showmenu(true)
    set_values("client")
option_end()

-- Bundle an existing ffmpeg.exe instead of building FFmpeg, whose build needs MSYS2 on a Windows host.
option("ffmpeg_exe")
    set_showmenu(true)
    set_description("Prebuilt ffmpeg.exe to bundle instead of building the playback-ffmpeg package")
option_end()

-- xmake (through 3.1.2) keeps clang-cl's /clang:-MMD depfile without joining its "\"-continued lines, so every
-- multi-line depfile lists a "\" dependency. On a Linux host that file never exists and every object recompiles on
-- every build. Join the lines the previous build left before xmake checks them (the prepare stage ends before the build
-- stage, which also builds the precompiled header), keeping each depfile's mtime, which xmake compares with the sources'.
rule("playback.depfiles")
    before_prepare(function (target)
        local objectfiles = table.join(target:objectfiles(), target:pcoutputfile("cxx") or {})
        for _, objectfile in ipairs(objectfiles) do
            local dependfile = target:dependfile(objectfile)
            local dependinfo = os.isfile(dependfile) and io.load(dependfile)
            local depfiles   = dependinfo and dependinfo.depfiles_format == "gcc" and dependinfo.depfiles
            if depfiles and depfiles:find("\\\n", 1, true) then
                local mtime = os.mtime(dependfile)
                dependinfo.depfiles = depfiles:gsub("\\\r?\n", "")
                io.save(dependfile, dependinfo)
                os.touch(dependfile, {mtime = mtime})
            end
        end
    end)
rule_end()

-- dev only, in place of @levibuildscript/linkrule. That rule runs prelink.exe (under Wine on Linux, about 25 s) on every
-- relink to generate bedrock_runtime_api.lib, the import stubs for the game symbols the objects use. The library only
-- depends on those symbols and the input libraries, so this runs prelink the same way but only when the objects'
-- undefined symbols or an input library change, and keeps the library between builds. Without llvm-nm it always runs.
rule("playback.devprelink")
    on_config(function (target)
        target:add("shflags", "/DELAYLOAD:bedrock_runtime.dll")
    end)
    before_link(function (target)
        import("lib.detect.find_file")
        import("lib.detect.find_tool")
        import("core.project.config")
        import("core.project.depend")

        local outdir  = path.join(config.builddir(), ".prelink")
        local libdir  = path.join(outdir, "lib")
        local libfile = path.join(libdir, "bedrock_runtime_api.lib")
        os.mkdir(libdir)

        local envs        = os.joinenvs(target:pkgenvs(), os.getenvs())
        local searchpaths = path.splitenv(envs.PATH or envs.Path or envs.path or "")
        local data        = assert(find_file("bedrock_runtime_data", searchpaths), "Cannot find bedrock_runtime_data")
        local prelink     = assert(find_file("prelink.exe", searchpaths), "Cannot find prelink.exe")

        local libraries = {}
        for _, dep in ipairs(target:orderdeps()) do
            if dep:kind() == "static" and os.isfile(dep:targetfile()) then
                table.insert(libraries, dep:targetfile())
            end
        end
        for _, pkg in ipairs(target:orderpkgs()) do
            for _, linkdir in ipairs(pkg:get("linkdirs")) do
                for _, link in pairs(pkg:get("links")) do
                    local libraryfile = path.join(linkdir, link .. ".lib")
                    if os.isfile(libraryfile) then
                        table.insert(libraries, libraryfile)
                    end
                end
            end
        end
        local inputs = table.join(target:objectfiles(), libraries)

        local symbols = "unknown"
        local nm      = find_tool("llvm-nm")
        if nm then
            local listing = os.iorunv(nm.program, table.join("--undefined-only", "--just-symbol-name", target:objectfiles()))
            local names   = {}
            for _, line in ipairs(listing:split("\n", {plain = true})) do
                if line ~= "" and not line:endswith(":") then
                    table.insert(names, line)
                end
            end
            table.sort(names)
            symbols = hash.strhash128(table.concat(names, "\n"))
        end

        depend.on_changed(function ()
            print("Running prelink...")
            os.execv(prelink, table.join(
                string.format("%s-%s-%s", config.get("target_type"), config.get("plat"), config.get("arch")),
                outdir, data, inputs))
        end, {
            dependfile = path.join(outdir, "prelink.d"),
            changed    = target:is_rebuilt() or not os.isfile(libfile) or symbols == "unknown",
            values     = {prelink, data, symbols},
            files      = libraries
        })

        target:add("linkdirs", libdir)
        target:add("links", "bedrock_runtime_api")
    end)
rule_end()

add_requires("levilamina 26.60.*", {configs = {target_type = get_config("target_type")}})

add_requires("levibuildscript")

add_requires("stduuid")
add_requires("xxhash")
add_requires("mbedtls v3.6.7")
add_requires("libzip")
add_requires("imgui v1.92.7", {configs = {dx11 = true, dx12 = true}})
add_requires("cpp-httplib")
if not has_config("ffmpeg_exe") then
    add_requires("playback-ffmpeg 7.1", {
        configs = {
            shared = false,
            gpl = true,
            ffmpeg = true,
            ffprobe = false,
            ffplay = false,
            libx264 = true,
        }
    })
end

if not has_config("vs_runtime") then
    set_runtimes("MD")
end

local get_version = function(os)
    local version_override      = os.getenv("PLAYBACK_VERSION")
    local has_version_override = version_override and version_override:match("%S") ~= nil
    local tag                   = has_version_override and version_override or os.iorun("git describe --tags --abbrev=0 --always")
    tag                         = (tag or ""):gsub("^%s+", ""):gsub("%s+$", "")

    local major, minor, patch, suffix = tag:match("^v?(%d+)%.(%d+)%.(%d+)(.*)$")
    if not major then
        local ci = os.getenv("CI")
        if has_version_override or ci == "true" or ci == "1" then
            os.raise("Unable to parse Playback version tag: " .. (tag ~= "" and tag or "<empty>"))
        end
        print("Failed to parse version tag, using 0.0.0")
        return "0.0.0"
    end

    if suffix ~= "" then return major .. "." .. minor .. "." .. patch .. suffix end
    return major .. "." .. minor .. "." .. patch
end

target("playback")
    if is_mode("dev") then
        add_rules("playback.devprelink")
    else
        add_rules("@levibuildscript/linkrule")
    end
    if is_plat("windows") then
        add_defines("NOMINMAX", "UNICODE")
        set_exceptions("none") -- To avoid conflicts with /EHa.
        add_cxflags( "/EHa", "/utf-8", "/W4", "/w44265", "/w44289", "/w44296", "/w45263", "/w44738", "/w45204")
        add_cxflags(
            "/EHs",
            "-Wno-microsoft-cast",
            "-Wno-invalid-offsetof",
            "-Wno-c++2b-extensions",
            "-Wno-microsoft-include",
            "-Wno-overloaded-virtual",
            "-Wno-ignored-qualifiers",
            "-Wno-missing-field-initializers",
            "-Wno-potentially-evaluated-expression",
            "-Wno-pragma-system-header-outside-header",
            {tools = {"clang_cl"}}
        )
        set_toolchains("clang-cl")
    end
    add_packages("levilamina")
    add_packages("stduuid")
    add_packages("xxhash")
    add_packages("mbedtls")
    add_packages("libzip")
    add_packages("imgui")
    add_packages("cpp-httplib")
    add_syslinks("d3d11", "d3d12", "dxgi", "dcomp", "d3dcompiler", "d2d1", "dwrite", "windowscodecs", "ole32", "comdlg32", "shell32", "gdi32", "ws2_32")
    set_kind("shared")
    set_languages("c++20")
    if is_mode("debug") then
        add_defines("DEBUG")
    end
    add_headerfiles("src/**.h")
    add_files("src/**.cpp")
    add_includedirs("src")
    set_pcxxheader("src/pch.h")
    add_rules("playback.depfiles")
    set_symbols("debug")
    on_load(function (target)
        target:add("rules", "@levibuildscript/modpacker", {
            modVersion = get_version(os),
        })
    end)
    after_build(function (target)
        import("utils.archive")
        import("core.project.project")

        local output_dir = path.join(os.projectdir(), "bin", target:name())
        os.mkdir(output_dir)
        os.cp(path.join(os.projectdir(), "LICENSE"), output_dir)
        os.cp(path.join(os.projectdir(), "THIRD_PARTY_NOTICES.md"), output_dir)

        local license_source = path.join(os.projectdir(), "licenses")
        local license_dir = path.join(output_dir, "licenses")
        os.tryrm(license_dir)
        os.cp(license_source, license_dir)

        local lang_source = path.join(os.projectdir(), "src", "lang")
        local lang_dir = path.join(output_dir, "lang")
        os.tryrm(lang_dir)
        os.cp(lang_source, lang_dir)

        local font_source = path.join(os.projectdir(), "assets", "fonts", "lucide.ttf")
        local font_dir    = path.join(output_dir, "fonts")
        assert(os.isfile(font_source), "icon font asset was not found")
        os.tryrm(font_dir)
        os.mkdir(font_dir)
        os.cp(font_source, font_dir)

        local ffmpeg_source = get_config("ffmpeg_exe")
        if not ffmpeg_source then
            local ffmpeg_package = assert(
                project.required_package("playback-ffmpeg"),
                "FFmpeg package was not resolved"
            )
            ffmpeg_source = path.join(ffmpeg_package:installdir(), "bin", "ffmpeg.exe")
        end
        local tools_dir      = path.join(output_dir, "tools")
        assert(os.isfile(ffmpeg_source), "bundled ffmpeg executable was not found")
        os.mkdir(tools_dir)
        os.cp(ffmpeg_source, path.join(tools_dir, "ffmpeg.exe"))

        local resource_dir = path.join(os.projectdir(), "resources")
        if os.isdir(resource_dir) then
            local installed_pack = path.join(output_dir, "resource_packs", target:name() .. "-ui")
            local mcpack         = path.join(os.projectdir(), "bin", target:name() .. "-ui.mcpack")
            local mcpack_zip     = mcpack .. ".zip"
            assert(os.isfile(path.join(resource_dir, "manifest.json")), "resource pack manifest.json was not found")
            assert(
                os.isfile(path.join(resource_dir, "ui", "start_screen.json")),
                "main-menu button resource was not found"
            )
            os.tryrm(installed_pack)
            os.cp(resource_dir, installed_pack)
            os.tryrm(mcpack)
            os.tryrm(mcpack_zip)
            archive.archive(mcpack_zip, "*", {
                curdir = resource_dir,
                recurse = true
            })
            os.mv(mcpack_zip, mcpack)
            cprint("${bright green}[Playback]: ${reset}Main-menu button resource pack installed to " .. installed_pack)
            cprint("${bright green}[Playback]: ${reset}Standalone UI resource pack generated to " .. mcpack)
        end
    end)
