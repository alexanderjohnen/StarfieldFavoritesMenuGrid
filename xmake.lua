set_xmakever("3.0.0")

includes("../CommonLibSF-libxse")

set_project("FavoritesMenuGrid")
set_version("1.0.2")
set_license("GPL-3.0-or-later")
set_arch("x64")
set_languages("c++23")
set_warnings("allextra")
set_encodings("utf-8")

add_rules("mode.debug", "mode.releasedbg")

target("FavoritesMenuGrid", function()
    add_rules("commonlibsf.plugin", {
        name = "Favorites Menu Grid",
        author = "LX6R",
        description = "Every favorites row at once, as a grid you can click and edit",
        options = {
            address_library = true,
            layout_dependent = true
        }
    })

    add_files("src/**.cpp")
    add_headerfiles("src/**.h")
    add_includedirs("src")
    set_pcxxheader("src/pch.h")
end)
