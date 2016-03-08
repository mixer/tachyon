project(obs-qtwebkit)

find_package(Qt5WebKit REQUIRED)
find_package(Threads REQUIRED)

if(WIN32)
	if(MSVC)
		set(libobs_PLATFORM_DEPS
		${libobs_PLATFORM_DEPS}
		w32-pthreads)
	endif()
endif(WIN32)

set(obs-qtwebkit_SOURCES
	qtwebkit-main.cpp
	qtwebkit-manager.cpp
	qtwebkit-manager.hpp
	qtwebkit-renderer.cpp
	qtwebkit-source.cpp
	qtwebkit-source.hpp)

add_library(obs-qtwebkit MODULE
	${obs-qtwebkit_SOURCES})
target_link_libraries(obs-qtwebkit
	libobs
	Qt5::WebKit
	${libobs_PLATFORM_DEPS})
	
install_obs_plugin_with_data(obs-qtwebkit data)
