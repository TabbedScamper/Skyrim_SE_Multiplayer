-- This function defines the core component idoms
function component(name)
  target(name)
    set_kind("static")
    set_group("Components")
    add_configfiles("BuildInfo.h.in")
    -- The release workflow sets SSM_RELEASE_VERSION from the tag (1.2.3); local builds leave it empty.
    set_configvar("SSM_RELEASE_VERSION", os.getenv("SSM_RELEASE_VERSION") or "")
    add_includedirs(
      ".",
      "../",
      "../../", 
      "../../../build", 
      {public = true})
    add_headerfiles("**.h")
    add_files("**.cpp")
    add_packages(
      "tiltedcore", 
      "hopscotch-map", 
      "gtest",
      "spdlog")
end

-- this isnt fully specified yet.
function unittest(name)
    target(name .. "_Tests")
      set_kind("binary")
      set_group("Tests")
      add_configfiles("BuildInfo.h.in")
      -- The release workflow sets SSM_RELEASE_VERSION from the tag (1.2.3); local builds leave it empty.
      set_configvar("SSM_RELEASE_VERSION", os.getenv("SSM_RELEASE_VERSION") or "")
      add_includedirs(
        ".",
        "../",
        "../../", 
        "../../../build", 
        {public = true})
      add_headerfiles(
          "**.h")
      add_files(
          "**.cpp",
          "../../TestMain.cpp")
      add_packages(
        "tiltedcore", 
        "hopscotch-map", 
        "gtest",
        "spdlog")
  end

-- List all components required below:
includes("console")
includes("imgui")
includes("es_loader")
includes("resources")
