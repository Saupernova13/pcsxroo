# SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
# SPDX-License-Identifier: GPL-3.0+

# PCSXROO's unit tests, as their own ctest target so upstream's core_test is untouched.
# They link the core like core_test does, and borrow its host stub rather than copying it.
add_pcsx2_test(pcsxroo_test
	debug_server_json_tests.cpp
	debugger_control_tests.cpp
	memory_search_tests.cpp
	pcsxroo_args_tests.cpp
	pcsxroo_render_tests.cpp
	${CMAKE_SOURCE_DIR}/pcsxroo/cli/Args.cpp
	${CMAKE_SOURCE_DIR}/pcsxroo/cli/Render.cpp
	${CMAKE_SOURCE_DIR}/tests/ctest/core/StubHost.cpp
)

target_include_directories(pcsxroo_test PRIVATE
	"${CMAKE_SOURCE_DIR}"
	"${CMAKE_SOURCE_DIR}/tests/ctest/core"
)

target_link_libraries(pcsxroo_test PUBLIC
	PCSX2_FLAGS
	PCSX2
	common
)

# Upstream's unittests target runs ctest only in tests/ctest, so it builds this suite (via
# add_pcsx2_test) but would never run it. Running it as it is built means `unittests`, and
# therefore CI on every platform, fails when a PCSXROO test does.
add_custom_command(TARGET pcsxroo_test POST_BUILD COMMAND pcsxroo_test)
