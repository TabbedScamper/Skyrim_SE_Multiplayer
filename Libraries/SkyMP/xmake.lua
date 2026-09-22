target("SkyMPPapyrusVM")
    set_kind("static")
    set_group("Libraries")
    add_files(
        "papyrus-vm/src/papyrus-vm-lib/*.cpp",
        "viet/src/Promise.cpp")
    add_headerfiles(
        "papyrus-vm/include/(papyrus-vm/*.h)",
        "viet/include/*.h",
        "compat/MakeID.h")
    add_includedirs(
        "papyrus-vm/include",
        "papyrus-vm/src",
        "viet/include",
        "compat",
        {public = true})
    add_packages("spdlog", "fmt")
    add_defines("SPDLOG_HEADER_ONLY")
    if is_plat("windows") then
        add_defines("WIN32")
    end
