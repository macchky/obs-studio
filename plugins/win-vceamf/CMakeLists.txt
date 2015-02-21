project(win-vceamf)

find_package(AMDAMF) # REQUIRED)
IF(AMDAMF_FOUND)
include_directories(${AMDAMF_INCLUDE_DIRS})

set(win-vceamf_HEADERS
	device-dx11.hpp
	device-dx9.hpp
	device-gl.hpp
	device-ocl.hpp)
	
set(win-vceamf_SOURCES
	win-vceamf.cpp
	win-vceamf-plugin-main.cpp
	device-dx11.cpp
	device-dx9.cpp
	device-gl.cpp
	device-ocl.cpp)

add_library(win-vceamf MODULE
	${win-vceamf_SOURCES}
	${win-vceamf_HEADERS})
target_link_libraries(win-vceamf
	libobs
	w32-pthreads
	d3d11
	dxgi
	${AMDAMF_LIBRARIES}
	)

install_obs_plugin_with_data(win-vceamf data)
ELSE(AMDAMF_FOUND)
	MESSAGE("VCE AMF encoder will not be built.")
ENDIF(AMDAMF_FOUND)