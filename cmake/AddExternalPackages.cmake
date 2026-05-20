include_guard(GLOBAL)

include(get_cpm)

macro(add_external_packages)
	CPMAddPackage(
		NAME arena
		GITHUB_REPOSITORY ccgargantua/arena-allocator
		GIT_TAG 5377b1636450a2ec077b9356532ead7422c71c20
		DOWNLOAD_ONLY YES
	)
	if (BUILD_COVERAGE)
		CPMAddPackage(
			NAME lcov-to-cobertura-xml
			GITHUB_REPOSITORY eriwen/lcov-to-cobertura-xml
			GIT_TAG 028da3798355d0260c6c6491b39347d84ca7a02d
			DOWNLOAD_ONLY YES
		)
	endif (BUILD_COVERAGE)
endmacro()
