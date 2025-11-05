include_guard(GLOBAL)

include(get_cpm)

macro(add_external_packages)
	if (BUILD_TESTING)
		# Note: defines ${greatest_SOURCE_DIR}
		CPMAddPackage(
			NAME greatest
			GITHUB_REPOSITORY silentbicycle/greatest
			VERSION 1.5.0
			DOWNLOAD_ONLY YES
		)
	endif (BUILD_TESTING)
	if (BUILD_COVERAGE)
		CPMAddPackage(
			NAME lcov-to-cobertura-xml
			GITHUB_REPOSITORY eriwen/lcov-to-cobertura-xml
			GIT_TAG 028da3798355d0260c6c6491b39347d84ca7a02d
			DOWNLOAD_ONLY YES
		)
	endif (BUILD_COVERAGE)
endmacro()
