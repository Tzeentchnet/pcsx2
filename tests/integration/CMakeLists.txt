if(NOT TARGET kzgs OR NOT TARGET kzgs_flags)
	message(FATAL_ERROR "These integration tests require a standalone KzGS build.")
endif()

set(_pcsx2_gtest "${CMAKE_CURRENT_LIST_DIR}/../../3rdparty/googletest/googletest")
add_library(pcsx2_review_gtest STATIC
	"${_pcsx2_gtest}/src/gtest-all.cc"
	"${_pcsx2_gtest}/src/gtest_main.cc"
)
target_include_directories(pcsx2_review_gtest PUBLIC "${_pcsx2_gtest}/include" PRIVATE "${_pcsx2_gtest}")
target_compile_features(pcsx2_review_gtest PRIVATE cxx_std_17)
target_compile_options(pcsx2_review_gtest PRIVATE /Zc:__cplusplus)

add_executable(pcsx2_kzgs_review_test
	"${CMAKE_CURRENT_LIST_DIR}/../ctest/core/GS/presentation_hook_tests.cpp"
	"${CMAKE_CURRENT_LIST_DIR}/../ctest/core/GS/temporal_observation_tests.cpp"
	"${CMAKE_CURRENT_LIST_DIR}/../ctest/core/GS/vulkan_feature_chain_tests.cpp"
	"${CMAKE_CURRENT_LIST_DIR}/presentation_integration_tests.cpp"
)
target_include_directories(pcsx2_kzgs_review_test PRIVATE "${KZGS_ROOT}/src")
target_link_libraries(pcsx2_kzgs_review_test PRIVATE kzgs kzgs_flags pcsx2_review_gtest)
target_precompile_headers(pcsx2_kzgs_review_test PRIVATE "${P2SRC}/PrecompiledHeader.h")
kzgs_stage_resources(pcsx2_kzgs_review_test)
