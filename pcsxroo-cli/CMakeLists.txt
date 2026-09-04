# SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
# SPDX-License-Identifier: GPL-3.0+

add_executable(pcsxroo-cli
	main.cpp
	Args.h
	Args.cpp
	Client.h
	Client.cpp
	Commands.h
	Commands.cpp
	Launch.h
	Launch.cpp
	Render.h
	Render.cpp
)

# Deliberately links neither PCSX2 nor Qt: the client needs a socket and a JSON parser, and
# nothing else. That keeps it startable when the emulator is not, which is the whole point
# of being able to report "cannot connect" rather than hanging.
target_link_libraries(pcsxroo-cli PRIVATE
	common
	rapidjson
	fmt::fmt
)

target_include_directories(pcsxroo-cli PRIVATE "${CMAKE_SOURCE_DIR}")
set_target_properties(pcsxroo-cli PROPERTIES OUTPUT_NAME "pcsxroo")

if(WIN32)
	target_link_libraries(pcsxroo-cli PRIVATE ws2_32)
endif()

# Installs beside the emulator, matching how updater/ does it, so that
# "pcsxroo launch" can find pcsxroo-qt.exe next to itself.
install(TARGETS pcsxroo-cli DESTINATION ${CMAKE_SOURCE_DIR}/bin)
