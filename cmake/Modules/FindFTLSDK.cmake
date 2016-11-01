# Once done these will be defined:
#
#  FTLSDK_FOUND
#  FTLSDK_INCLUDE_DIRS
#  FTLSDK_LIBRARIES
#
# For use in OBS:
#
#  FTLSDK_INCLUDE_DIR

find_package(PkgConfig QUIET)
if (PKG_CONFIG_FOUND)
	pkg_check_modules(_FTL QUIET ftl libftl)
endif()

if(CMAKE_SIZEOF_VOID_P EQUAL 8)
	set(_lib_suffix 64)
else()
	set(_lib_suffix 32)
endif()

find_path(FTLSDK_INCLUDE_DIR
	NAMES ftl.h
	HINTS
		ENV FTLPath${_lib_suffix}
		ENV FTLPath
		ENV DepsPath${_lib_suffix}
		ENV DepsPath
		${FTLPath${_lib_suffix}}
		${FTLPath}
		${DepsPath${_lib_suffix}}
		${DepsPath}
		${_FTL_INCLUDE_DIRS}
	PATHS
		/usr/include /usr/local/include /opt/local/include /sw/include
	PATH_SUFFIXES
		include)

find_library(FTLSDK_LIB
	NAMES ${_FTLSDK_LIBRARIES} ftl libftl
	HINTS
		ENV FTLPath${_lib_suffix}
		ENV FTLPath
		ENV DepsPath${_lib_suffix}
		ENV DepsPath
		${FTLPath${_lib_suffix}}
		${FTLPath}
		${DepsPath${_lib_suffix}}
		${DepsPath}
		${_FTLSDK_LIBRARY_DIRS}
	PATHS
		/usr/lib /usr/local/lib /opt/local/lib /sw/lib
	PATH_SUFFIXES
		lib${_lib_suffix} lib
		libs${_lib_suffix} libs
		bin${_lib_suffix} bin
		../lib${_lib_suffix} ../lib
		../libs${_lib_suffix} ../libs
		../bin${_lib_suffix} ../bin)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(FTLSDK DEFAULT_MSG FTLSDK_LIB FTLSDK_INCLUDE_DIR)
mark_as_advanced(FTLSDK_INCLUDE_DIR FTLSDK_LIB)

if(FTLSDK_FOUND)
	set(FTLSDK_INCLUDE_DIRS ${FTLSDK_INCLUDE_DIR})
	set(FTLSDK_LIBRARIES ${FTLSDK_LIB})
endif()
