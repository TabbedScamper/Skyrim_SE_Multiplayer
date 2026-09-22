target("SkyrimProtocolBot")
    set_kind("binary")
    set_group("Tests")
    set_symbols("debug", "hidden")
    add_files("main.cpp")
    add_includedirs(".", "../", "../../Libraries/")
    add_deps("BaseLib", "SkyrimEncoding", "TiltedConnect")
    add_packages(
        "tiltedcore",
        "hopscotch-map",
        "gamenetworkingsockets",
        "libuv",
        "snappy",
        "spdlog",
        "glm")
    add_defines("SPDLOG_HEADER_ONLY")
