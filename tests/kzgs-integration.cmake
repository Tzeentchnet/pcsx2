# Inject the companion tests into a standalone KzGS build with
# -DCMAKE_PROJECT_kzgs_INCLUDE=<absolute-path-to-this-file>, then build and run
# pcsx2_kzgs_review_test. The integration cases require Windows, Vulkan validation,
# and D3D11/D3D12 WARP; the non-indexed Vulkan case also supports sync validation.
set(PCSX2_REVIEW_TEST_DIR "${CMAKE_CURRENT_LIST_DIR}/integration")
function(pcsx2_add_kzgs_integration_tests)
	include("${PCSX2_REVIEW_TEST_DIR}/CMakeLists.txt")
endfunction()
cmake_language(DEFER CALL pcsx2_add_kzgs_integration_tests)
