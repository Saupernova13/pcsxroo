# SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
# SPDX-License-Identifier: GPL-3.0+

# Everything PCSXROO adds to PCSX2 lives under this directory. The debug server itself is
# compiled into the core from pcsx2/CMakeLists.txt, because it needs the core's headers and
# its link; this file adds the parts that stand on their own.

add_subdirectory(cli)

if(ENABLE_TESTS)
	add_subdirectory(tests)
endif()
