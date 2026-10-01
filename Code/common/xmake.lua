
target("CommonLib")
    add_configfiles("BuildInfo.h.in")
    -- The release workflow sets SSM_RELEASE_VERSION from the tag (1.2.3); local builds leave it empty.
    set_configvar("SSM_RELEASE_VERSION", os.getenv("SSM_RELEASE_VERSION") or "")
    set_kind("static")
    set_group("common")
    add_includedirs(".", "../", "../../build", {public = true})
    add_headerfiles("**.h")
    add_files("**.cpp")
    add_packages("tiltedcore", "hopscotch-map")
