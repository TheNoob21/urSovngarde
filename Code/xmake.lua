if is_plat("windows") then
    for _, addon in ipairs(os.dirs(path.join(os.scriptdir(), "addons", "*"))) do
        if os.isfile(path.join(addon, "xmake.lua")) then
            includes(addon)
        end
    end
    includes("client")
    includes("immersive_elf")
    includes("immersive_launcher")
    includes("tp_process")
    target("Vanilla")
        set_kind("phony")
        set_default(false)
        add_deps("SkyrimImmersiveLauncher", "SkyrimImmersiveLauncherVR", "SkyrimServerRunner", "TPProcess")
    target_end()
end

includes("common")
includes("components")
includes("base")
includes("admin_protocol")
includes("server_runner")
includes("server")
includes("encoding")
includes("tests")
includes("bot")
