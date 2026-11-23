-- TODO (Force): please align into namespace
lib_dir = os.curdir()
directx_dir = lib_dir .. "/DXSDK"

if is_plat("windows") then
	includes("./TiltedUI")
	includes("./TiltedReverse")
	includes("./TiltedHooks")

	-- TiltedHooks' DInputHook.cpp is built from urSovngarde's copy instead (overrides/TiltedHooks): under Wine it hooks
	-- CreateDevice in its vtable slot, where the submodule's code patch broke Wine's ARM64EC dinput8.dll on the Quest.
	-- The submodule stays at upstream's commit.
	target("TiltedHooks")
		remove_files("TiltedHooks/Code/hooks/src/DInputHook.cpp")
		add_files("overrides/TiltedHooks/DInputHook.cpp", {unity_ignored = true})
		add_includedirs("../Code")
		add_packages("spdlog")
	target_end()
end

includes("./TiltedConnect")
