# Once done these will be defined:
#
#  AMDAPP_FOUND
#  AMDAPP_INCLUDE_DIRS
#  AMDAPP_LIBRARIES

# Only finds Windows' libs
# Get environment variable, define it as ENV_$var and make sure backslashes are converted to forward slashes
macro(getenv_path VAR)
	set(ENV_${VAR} $ENV{${VAR}})
	# replace won't work if var is blank
	if (ENV_${VAR})
		#Didn't work for me
		#string( REGEX REPLACE "\\\\" "/" ENV_${VAR} ${ENV_${VAR}} )
		string( REGEX REPLACE "\\\\" "/" ${VAR} ${ENV_${VAR}} )
	endif ()
endmacro(getenv_path)

getenv_path(AMDAPPSDKROOT)

if(CMAKE_SIZEOF_VOID_P EQUAL 8)
	set(_lib_suffix 64)
	set(_arch_dir x86_64)
else()
	set(_lib_suffix 32)
	set(_arch_dir x86)
endif()

SET(APPSDK_HINTS 
	"${AMDAPPSDKROOT}"
	"../../../AMD APP SDK/3.0-0"
	"../../../AMD APP SDK/3.0-0-Beta"
	"../../../AMD APP SDK/2.9-1")
	
find_path(APPSDK_INCLUDE_DIR
	NAMES CL/cl.hpp
	HINTS
		${APPSDK_HINTS}
	PATHS
		/usr/include /usr/local/include /opt/local/include /sw/include
	PATH_SUFFIXES
		include)

find_library(APPSDK_LIB
	NAMES OpenCL
	HINTS
		${APPSDK_HINTS}
	PATHS
		/usr/lib /usr/local/lib /opt/local/lib /sw/lib
	PATH_SUFFIXES
		lib/${_arch_dir}
		../lib/${_arch_dir})
		
include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(AMDAPP DEFAULT_MSG APPSDK_LIB APPSDK_INCLUDE_DIR)
mark_as_advanced(APPSDK_INCLUDE_DIR APPSDK_LIB)

if(AMDAPP_FOUND)
	set(AMDAPP_INCLUDE_DIRS ${APPSDK_INCLUDE_DIR})
	set(AMDAPP_LIBRARIES ${APPSDK_LIB})
	mark_as_advanced(AMDAPP_LIBRARIES)
endif()
