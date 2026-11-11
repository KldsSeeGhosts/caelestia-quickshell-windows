cmake_minimum_required(VERSION 3.20)
project(qs_windows_panel_animation_test LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 6.11 REQUIRED COMPONENTS Gui GuiPrivate Quick QuickPrivate Qml Test)
qt_standard_project_setup(REQUIRES 6.11)

set(QS_WINDOWS_SRC "${CMAKE_CURRENT_SOURCE_DIR}/..")

# Qt 6.11 lets CMake merge the executable manifest with mt.exe. On current
# Windows builds that merge rewrites trustInfo/requestedExecutionLevel with
# ms_asmv1-prefixed elements and attributes, and the loader then rejects the
# executable with "side-by-side configuration is incorrect". Replace resource
# #1 with a plain asInvoker manifest after every link, so all fixtures below
# start under clang-cl/lld-link without a manual mt step.
set(QS_TEST_MANIFEST "${CMAKE_CURRENT_SOURCE_DIR}/test-as-invoker.manifest")
# Use the Windows SDK mt.exe, not CMAKE_MT: under clang-cl CMake picks
# llvm-mt, which ignores -outputresource and still exits 0.
set(QS_PF86 "ProgramFiles(x86)")
file(TO_CMAKE_PATH "$ENV{${QS_PF86}}" QS_PF86_DIR)
file(GLOB QS_SDK_BIN_DIRS LIST_DIRECTORIES true "${QS_PF86_DIR}/Windows Kits/10/bin/10.*/x64")
list(SORT QS_SDK_BIN_DIRS COMPARE NATURAL ORDER DESCENDING)
find_program(QS_SDK_MT NAMES mt.exe
	HINTS "$ENV{WindowsSdkVerBinPath}x64" ${QS_SDK_BIN_DIRS}
	NO_DEFAULT_PATH REQUIRED)

# These translation units have compatible definitions across their consumers.
# Each archive contains one source (and its moc, where needed), so linking
# it cannot pull unrelated platform implementations across a fake boundary.
set(QS_SHARED_TEST_SOURCES
	../core/redact ../core/model ../core/qmlscreen ../core/instanceinfo
	activation_policy window_selectors transition_trace util screen_identity
	tiling_scope tiling_layout system_surface fullscreen_occluders
	layer_rules panel_effects panel_animation startup input_settle
	capture_device capture_format capture_texture
	system/brightness system/gamma services/pipewire/pw_node services/pipewire/pw_types
	services/message_window native_desktop_accessor virtual_desktops desktop_host tiling
	system/network system/network_wifi_item
)
set(QS_SHARED_TEST_MOC_SOURCES
	../core/model ../core/qmlscreen screen_identity startup fullscreen_occluders
	tiling_scope panel_effects virtual_desktops desktop_host tiling
	system/brightness system/gamma system/network system/network_wifi_item
	services/pipewire/pw_node services/pipewire/pw_types
)
foreach(source IN LISTS QS_SHARED_TEST_SOURCES)
	string(MAKE_C_IDENTIFIER "fixture_${source}" library)
	get_filename_component(path "${QS_WINDOWS_SRC}/${source}.cpp" ABSOLUTE)
	add_library(${library} STATIC "${path}")
	# Avoid an empty moc translation unit for helpers without QObject classes.
	if(NOT source IN_LIST QS_SHARED_TEST_MOC_SOURCES)
		set_target_properties(${library} PROPERTIES AUTOMOC OFF)
	endif()
	set_target_properties(${library} PROPERTIES AUTOUIC OFF)
	target_compile_definitions(${library} PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
	# All consumers of these sources use precisely this test seam. Stubs remain
	# executable-local. Never reuse these archives in a production target.
	if(source MATCHES "^(native_desktop_accessor|virtual_desktops|desktop_host|system/network)$")
		target_compile_definitions(${library} PRIVATE QS_TEST)
	elseif(source STREQUAL "tiling")
		target_compile_definitions(${library} PRIVATE QS_TILING_FAKE_DESKTOP)
	endif()
	target_include_directories(${library} PRIVATE "${QS_WINDOWS_SRC}" "${QS_WINDOWS_SRC}/../core")
	target_link_libraries(${library} PRIVATE Qt6::GuiPrivate Qt6::QuickPrivate Qt6::Qml)
	set_property(GLOBAL APPEND PROPERTY QS_SHARED_TEST_PATHS "${path}")
	set_property(GLOBAL APPEND PROPERTY QS_SHARED_TEST_LIBRARIES "${library}")
endforeach()

function(qs_add_test_executable target)
	get_property(paths GLOBAL PROPERTY QS_SHARED_TEST_PATHS)
	get_property(libraries GLOBAL PROPERTY QS_SHARED_TEST_LIBRARIES)
	set(sources "")
	set(shared "")
	foreach(source IN LISTS ARGN)
		get_filename_component(path "${source}" ABSOLUTE)
		# Also move explicitly listed matching headers to the archive's automoc.
		string(REGEX REPLACE "[.]hpp$" ".cpp" path "${path}")
		list(FIND paths "${path}" index)
		if(index EQUAL -1)
			list(APPEND sources "${source}")
		else()
			list(GET libraries ${index} library)
			list(APPEND shared "${library}")
		endif()
	endforeach()
	list(REMOVE_DUPLICATES shared)
	qt_add_executable(${target} ${sources})
	target_link_libraries(${target} PRIVATE ${shared})
	add_custom_command(TARGET ${target} POST_BUILD
		COMMAND "${QS_SDK_MT}" -nologo -manifest "${QS_TEST_MANIFEST}"
			"-outputresource:$<TARGET_FILE:${target}>;#1"
		COMMENT "Embedding plain asInvoker manifest in ${target}"
		VERBATIM
	)
endfunction()

qs_add_test_executable(wallpaper_target_test wallpaper_target_test.cpp)
target_link_libraries(wallpaper_target_test PRIVATE Qt6::Gui Qt6::Test)
add_test(NAME wallpaper_target COMMAND wallpaper_target_test)

qs_add_test_executable(screen_identity_test
	screen_identity_test.cpp
	${QS_WINDOWS_SRC}/screen_identity.cpp
	${QS_WINDOWS_SRC}/../core/qmlscreen.cpp
	${QS_WINDOWS_SRC}/../core/redact.cpp
)
target_compile_definitions(screen_identity_test PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(screen_identity_test PRIVATE Qt6::GuiPrivate Qt6::Qml Qt6::Test user32)
add_test(NAME screen_identity COMMAND screen_identity_test)
set_tests_properties(screen_identity PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# The monitor keeps a QPointer to its QScreen because a hot-unplug destroys the
# screen before the topology timer removes the monitor. This fixture destroys a
# fake screen and calls both readers; without the guard they dereference freed
# memory. No HyprlandIpc singleton is constructed, so no desktop is observed.
qs_add_test_executable(hyprland_monitor_test
	hyprland_monitor_test.cpp
	hyprland_monitor_test_stubs.cpp
	${QS_WINDOWS_SRC}/hyprland/monitor.cpp
	${QS_WINDOWS_SRC}/hyprland/monitor.hpp
	${QS_WINDOWS_SRC}/hyprland/workspace.hpp
	${QS_WINDOWS_SRC}/../core/model.cpp
	${QS_WINDOWS_SRC}/../core/redact.cpp
	${QS_WINDOWS_SRC}/fullscreen_occluders.cpp
	${QS_WINDOWS_SRC}/screen_identity.cpp
	${QS_WINDOWS_SRC}/activation_policy.cpp
	${QS_WINDOWS_SRC}/util.cpp
)
target_compile_definitions(hyprland_monitor_test PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_include_directories(hyprland_monitor_test PRIVATE ${QS_WINDOWS_SRC})
target_link_libraries(hyprland_monitor_test PRIVATE
	Qt6::GuiPrivate Qt6::Qml Qt6::Test user32 dwmapi advapi32 ole32
)
add_test(NAME hyprland_monitor COMMAND hyprland_monitor_test)
set_tests_properties(hyprland_monitor PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

qs_add_test_executable(panel_animation_test
	panel_animation_test.cpp
	${QS_WINDOWS_SRC}/panel_animation.cpp
	${QS_WINDOWS_SRC}/panel_effects.cpp
	${QS_WINDOWS_SRC}/layer_rules.cpp
)

qt_add_resources(panel_animation_test "panel-effects"
	PREFIX "/Quickshell/Windows"
	BASE ${QS_WINDOWS_SRC}
	FILES ${QS_WINDOWS_SRC}/shaders/panel_effect.frag.qsb
)

target_include_directories(panel_animation_test PRIVATE ${QS_WINDOWS_SRC})
target_compile_definitions(panel_animation_test PRIVATE
	QS_WINDOWS_SHADER_SOURCE="${QS_WINDOWS_SRC}/shaders/panel_effect.frag"
	QS_WINDOWS_SHADER_QSB="${QS_WINDOWS_SRC}/shaders/panel_effect.frag.qsb"
)
target_link_libraries(panel_animation_test PRIVATE Qt6::Quick Qt6::Qml Qt6::Test Qt6::GuiPrivate)
add_test(NAME panel_animation COMMAND panel_animation_test)
set_tests_properties(panel_animation PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

qs_add_test_executable(screenshot_jobs_test
	screenshot_jobs_test.cpp
	${QS_WINDOWS_SRC}/system/screenshot_jobs.cpp
	${QS_WINDOWS_SRC}/../core/redact.cpp
)

target_include_directories(screenshot_jobs_test PRIVATE ${QS_WINDOWS_SRC}/system)
target_link_libraries(screenshot_jobs_test PRIVATE Qt6::Gui Qt6::Test)
add_test(NAME screenshot_jobs COMMAND screenshot_jobs_test)
set_tests_properties(screenshot_jobs PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# The listing runs on a worker and answers through listDirFinished. An offline
# file stands in for a cloud placeholder: it is still listed, marked onDisk
# false, so the UI can skip its thumbnail until it is opened.
qs_add_test_executable(fs_utils_test
	fs_utils_test.cpp
	${QS_WINDOWS_SRC}/system/fs_utils.cpp
)
target_compile_definitions(fs_utils_test PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_include_directories(fs_utils_test PRIVATE ${QS_WINDOWS_SRC}/system)
target_link_libraries(fs_utils_test PRIVATE Qt6::Core Qt6::Qml Qt6::Test)
add_test(NAME fs_utils COMMAND fs_utils_test)

find_package(Qt6 REQUIRED COMPONENTS Network)
qs_add_test_executable(ipc_limits_test ipc_limits_test.cpp)
target_link_libraries(ipc_limits_test PRIVATE Qt6::Core Qt6::Network Qt6::Test)
add_test(NAME ipc_limits COMMAND ipc_limits_test)
# Compile the production integration too, without starting a shell or linking
# the command handlers' generation/runtime dependencies into this fixture.
add_library(ipc_compile_check OBJECT ${QS_WINDOWS_SRC}/../ipc/ipc.cpp)
target_link_libraries(ipc_compile_check PRIVATE Qt6::Quick Qt6::Network)

# Capture color policy: one explicit 8-bit SDR format through the WGC frame
# pool, the shared texture and the Qt consumer, with the tested SDR fallback.
# No capture session and no desktop is observed.
qs_add_test_executable(capture_format_test
	capture_format_test.cpp
	${QS_WINDOWS_SRC}/capture_format.cpp
	${QS_WINDOWS_SRC}/capture_texture.cpp
)
target_compile_definitions(capture_format_test PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_include_directories(capture_format_test PRIVATE ${QS_WINDOWS_SRC})
target_link_libraries(capture_format_test PRIVATE
	Qt6::Gui Qt6::GuiPrivate Qt6::Quick Qt6::QuickPrivate Qt6::Test d3d11 windowsapp ole32
)
add_test(NAME capture_format COMMAND capture_format_test)
set_tests_properties(capture_format PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# Capture device recovery: a removal reported by the device or by a capture call
# releases the device, and the next request creates one. The factory and the
# removal probe are injected, so no GPU reset is needed.
qs_add_test_executable(capture_device_test
	capture_device_test.cpp
	${QS_WINDOWS_SRC}/capture_device.cpp
)
target_compile_definitions(capture_device_test PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_include_directories(capture_device_test PRIVATE ${QS_WINDOWS_SRC})
target_link_libraries(capture_device_test PRIVATE
	Qt6::Core Qt6::Test d3d11 windowsapp ole32
)
add_test(NAME capture_device COMMAND capture_device_test)
set_tests_properties(capture_device PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# The real VideoNode from video_wallpaper.cpp, compiled in as one unit because
# the node lives in an unnamed namespace. A never-shown render-control window
# supplies the Direct3D 11 scene graph; synthetic shared frames replace the
# decoder. A replaced import must survive the swap. No media or desktop is used.
qs_add_test_executable(video_node_test
	video_node_test.cpp
	${QS_WINDOWS_SRC}/video_wallpaper.hpp
	${QS_WINDOWS_SRC}/capture.cpp
	${QS_WINDOWS_SRC}/capture_device.cpp
	${QS_WINDOWS_SRC}/capture_format.cpp
	${QS_WINDOWS_SRC}/capture_texture.cpp
	${QS_WINDOWS_SRC}/../core/redact.cpp
)
qt_generate_moc(${QS_WINDOWS_SRC}/video_wallpaper.cpp
	${CMAKE_CURRENT_BINARY_DIR}/video_wallpaper.moc TARGET video_node_test)
target_sources(video_node_test PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/video_wallpaper.moc)
target_compile_definitions(video_node_test PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_include_directories(video_node_test PRIVATE ${QS_WINDOWS_SRC} ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(video_node_test PRIVATE
	Qt6::Gui Qt6::GuiPrivate Qt6::Quick Qt6::QuickPrivate Qt6::Qml Qt6::Test
	d3d11 windowsapp ole32 mfplat mf mfuuid oleaut32
)
add_test(NAME video_node COMMAND video_node_test)
set_tests_properties(video_node PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=rhi")

# The redaction helper itself: salted, per-process and opaque.
qs_add_test_executable(redaction_test
	redaction_test.cpp
	${QS_WINDOWS_SRC}/../core/redact.cpp
)
target_compile_definitions(redaction_test PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_include_directories(redaction_test PRIVATE ${QS_WINDOWS_SRC})
target_link_libraries(redaction_test PRIVATE Qt6::Core Qt6::Test)
add_test(NAME redaction COMMAND redaction_test)

# The central log-sink scrubber: profile paths and the account name never reach
# a sink verbatim.
qs_add_test_executable(log_scrub_test
	log_scrub_test.cpp
	${QS_WINDOWS_SRC}/../core/logscrub.cpp
)
target_compile_definitions(log_scrub_test PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_include_directories(log_scrub_test PRIVATE ${QS_WINDOWS_SRC}/../core)
target_link_libraries(log_scrub_test PRIVATE Qt6::Core Qt6::Test)
add_test(NAME log_scrub COMMAND log_scrub_test)

# Source guard: no MAC-like literal or raw user/device/app identifier may be
# streamed to a log from src/windows, src/core or src/io. Snippet fixtures pin
# the identifiers the audit found, so a new unsafe log statement fails the
# backend suite without a live desktop.
get_filename_component(QS_BACKEND_SRC "${QS_WINDOWS_SRC}/../.." ABSOLUTE)
qs_add_test_executable(log_redaction_guard_test log_redaction_guard_test.cpp)
target_compile_definitions(log_redaction_guard_test PRIVATE
	NOMINMAX WIN32_LEAN_AND_MEAN
	QS_WINDOWS_SRC="${QS_WINDOWS_SRC}"
	QS_BACKEND_SRC="${QS_BACKEND_SRC}"
)
target_link_libraries(log_redaction_guard_test PRIVATE Qt6::Core Qt6::Test)
add_test(NAME log_redaction_guard COMMAND log_redaction_guard_test)

qs_add_test_executable(tiling_layout_test
	tiling_layout_test.cpp
	${QS_WINDOWS_SRC}/tiling_layout.cpp
)

target_include_directories(tiling_layout_test PRIVATE ${QS_WINDOWS_SRC})
target_link_libraries(tiling_layout_test PRIVATE Qt6::Core Qt6::Test)
add_test(NAME tiling_layout COMMAND tiling_layout_test)

# Workspace/resize/pin argument parsing and the pin dispatch seam. No HWND,
# tracker, tiler or desktop access. Monitor-focus selection and activation
# fixtures also use only a command-local monitor view and fake tracked windows.
qs_add_test_executable(dispatcher_test
	dispatcher_test.cpp
	${QS_WINDOWS_SRC}/hyprland/dispatcher_args.hpp
	${QS_WINDOWS_SRC}/hyprland/pin_support.hpp
	${QS_WINDOWS_SRC}/hyprland/monitor_focus.hpp
	${QS_WINDOWS_SRC}/hyprland/window_cycle.hpp
)

target_compile_definitions(dispatcher_test PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_include_directories(dispatcher_test PRIVATE ${QS_WINDOWS_SRC})
target_link_libraries(dispatcher_test PRIVATE Qt6::Core Qt6::Test)
add_test(NAME dispatcher COMMAND dispatcher_test)

# Compile the native dispatcher too: the pure fixture above deliberately does
# not link its platform singletons or execute any real activation.
add_library(dispatcher_compile_check OBJECT ${QS_WINDOWS_SRC}/hyprland/dispatcher.cpp)
target_compile_definitions(dispatcher_compile_check PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(dispatcher_compile_check PRIVATE Qt6::Quick Qt6::Qml)

# Policy and telemetry seams only: no HWND, no tracker, no desktop access.
qs_add_test_executable(tiling_observability_test
	tiling_observability_test.cpp
	${QS_WINDOWS_SRC}/tiling_scope.cpp
	${QS_WINDOWS_SRC}/window_selectors.cpp
	${QS_WINDOWS_SRC}/accessor_state.hpp
	${QS_WINDOWS_SRC}/hyprland/pin_support.hpp
)

target_compile_definitions(tiling_observability_test PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_include_directories(tiling_observability_test PRIVATE ${QS_WINDOWS_SRC})
target_link_libraries(tiling_observability_test PRIVATE Qt6::Core Qt6::Test)

# Real manager and layout engine, compiled against the fake desktop boundary.
# Never link native window_tracker/virtual_desktops/util into this executable.
qs_add_test_executable(tiling_manager_test
	tiling_manager_test.cpp
	tiling_fake_desktop.hpp
	${QS_WINDOWS_SRC}/tiling.cpp
	${QS_WINDOWS_SRC}/transition_trace.cpp
	${QS_WINDOWS_SRC}/../core/redact.cpp
	${QS_WINDOWS_SRC}/tiling.hpp
	${QS_WINDOWS_SRC}/tiling_scope.cpp
	${QS_WINDOWS_SRC}/window_selectors.cpp
	${QS_WINDOWS_SRC}/tiling_layout.cpp
	${QS_WINDOWS_SRC}/fullscreen_geometry.hpp
	${QS_WINDOWS_SRC}/accessor_state.hpp
	${QS_WINDOWS_SRC}/desktop_index.hpp
	${QS_WINDOWS_SRC}/hyprland/pin_support.hpp
)
target_compile_definitions(tiling_manager_test PRIVATE
	QS_TILING_FAKE_DESKTOP NOMINMAX WIN32_LEAN_AND_MEAN)
target_include_directories(tiling_manager_test PRIVATE ${QS_WINDOWS_SRC})
# The two-monitor fixture adds a QScreen through the QPA test interface.
target_link_libraries(tiling_manager_test PRIVATE Qt6::Gui Qt6::GuiPrivate Qt6::Qml Qt6::Test)

# Drag and drop of tiled windows: drop plan, preview geometry and commit,
# against the same fake desktop boundary as tiling_manager_test.
qs_add_test_executable(tiling_drag_test
	tiling_drag_test.cpp
	tiling_fake_desktop.hpp
	${QS_WINDOWS_SRC}/tiling.cpp
	${QS_WINDOWS_SRC}/transition_trace.cpp
	${QS_WINDOWS_SRC}/../core/redact.cpp
	${QS_WINDOWS_SRC}/tiling.hpp
	${QS_WINDOWS_SRC}/tiling_scope.cpp
	${QS_WINDOWS_SRC}/window_selectors.cpp
	${QS_WINDOWS_SRC}/tiling_layout.cpp
	${QS_WINDOWS_SRC}/fullscreen_geometry.hpp
	${QS_WINDOWS_SRC}/accessor_state.hpp
	${QS_WINDOWS_SRC}/desktop_index.hpp
	${QS_WINDOWS_SRC}/hyprland/pin_support.hpp
)
target_compile_definitions(tiling_drag_test PRIVATE
	QS_TILING_FAKE_DESKTOP NOMINMAX WIN32_LEAN_AND_MEAN)
target_include_directories(tiling_drag_test PRIVATE ${QS_WINDOWS_SRC})
target_link_libraries(tiling_drag_test PRIVATE Qt6::Gui Qt6::GuiPrivate Qt6::Qml Qt6::Test)

# Reload lifecycle over real QQmlEngines: each engine's `Tiling` singleton is
# one facade generation, and the process-lifetime manager must keep serving the
# newest one. Offscreen only; the fake desktop is the same boundary.
qs_add_test_executable(tiling_reload_test
	tiling_reload_test.cpp
	tiling_fake_desktop.hpp
	${QS_WINDOWS_SRC}/tiling.cpp
	${QS_WINDOWS_SRC}/transition_trace.cpp
	${QS_WINDOWS_SRC}/../core/redact.cpp
	${QS_WINDOWS_SRC}/tiling.hpp
	${QS_WINDOWS_SRC}/tiling_scope.cpp
	${QS_WINDOWS_SRC}/window_selectors.cpp
	${QS_WINDOWS_SRC}/tiling_layout.cpp
	${QS_WINDOWS_SRC}/fullscreen_geometry.hpp
	${QS_WINDOWS_SRC}/accessor_state.hpp
	${QS_WINDOWS_SRC}/desktop_index.hpp
	${QS_WINDOWS_SRC}/hyprland/pin_support.hpp
)
target_compile_definitions(tiling_reload_test PRIVATE
	QS_TILING_FAKE_DESKTOP NOMINMAX WIN32_LEAN_AND_MEAN)
target_include_directories(tiling_reload_test PRIVATE ${QS_WINDOWS_SRC})
target_link_libraries(tiling_reload_test PRIVATE Qt6::Gui Qt6::GuiPrivate Qt6::Qml Qt6::Test)

qs_add_test_executable(pw_audio_control_test
	pw_audio_control_test.cpp
	${QS_WINDOWS_SRC}/services/pipewire/pw_node.cpp
	${QS_WINDOWS_SRC}/services/pipewire/pw_types.cpp
	${QS_WINDOWS_SRC}/services/pipewire/policy_config.cpp
	${QS_WINDOWS_SRC}/../core/redact.cpp
)

target_compile_definitions(pw_audio_control_test PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_include_directories(pw_audio_control_test PRIVATE ${QS_WINDOWS_SRC}/services/pipewire)
target_link_libraries(pw_audio_control_test PRIVATE Qt6::Qml Qt6::Test ole32)

qs_add_test_executable(upower_status_test
	upower_status_test.cpp
	${QS_WINDOWS_SRC}/services/upower/device.cpp
)

target_compile_definitions(upower_status_test PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_include_directories(upower_status_test PRIVATE ${QS_WINDOWS_SRC}/services/upower)
target_link_libraries(upower_status_test PRIVATE Qt6::Qml Qt6::Test)

# Pairing request args, completion ordering and radio results use fakes through
# production seams. The full backend is compiled, but no backend thread starts
# and no WinRT device is touched at runtime.
qs_add_test_executable(bt_pairing_policy_test
	bt_pairing_policy_test.cpp
	${QS_WINDOWS_SRC}/bluetooth/pairing_policy.cpp
	${QS_WINDOWS_SRC}/bluetooth/device.cpp
	${QS_WINDOWS_SRC}/bluetooth/adapter.cpp
	${QS_WINDOWS_SRC}/bluetooth/bluetooth.cpp
	${QS_WINDOWS_SRC}/bluetooth/bt_backend.cpp
	${QS_WINDOWS_SRC}/bluetooth/bt_worker.cpp
	${QS_WINDOWS_SRC}/bluetooth/ks_audio.cpp
	${QS_WINDOWS_SRC}/bluetooth/device_class.cpp
	${QS_WINDOWS_SRC}/startup.cpp
	${QS_WINDOWS_SRC}/../core/redact.cpp
	${QS_WINDOWS_SRC}/../core/model.cpp
)

target_compile_definitions(bt_pairing_policy_test PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_include_directories(bt_pairing_policy_test PRIVATE
	${QS_WINDOWS_SRC}
	${QS_WINDOWS_SRC}/bluetooth
	${QS_WINDOWS_SRC}/../core
)
target_link_libraries(bt_pairing_policy_test PRIVATE
	Qt6::Quick
	Qt6::Qml
	Qt6::Test
	windowsapp
	runtimeobject
	ole32
	uuid
)

# No DLL and no keyboard hook: the test injects a fake accessor table and a
# recording chord sender. Production code must refuse desktop control without
# the accessor instead of synthesizing a Ctrl+Win chord.
qs_add_test_executable(virtual_desktops_test
	virtual_desktops_test.cpp
	${QS_WINDOWS_SRC}/native_desktop_accessor.cpp
	${QS_WINDOWS_SRC}/services/message_window.cpp
	${QS_WINDOWS_SRC}/virtual_desktops.cpp
	${QS_WINDOWS_SRC}/../core/redact.cpp
	${QS_WINDOWS_SRC}/accessor_state.hpp
	${QS_WINDOWS_SRC}/desktop_index.hpp
	${QS_WINDOWS_SRC}/hyprland/pin_support.hpp
)

target_compile_definitions(virtual_desktops_test PRIVATE QS_TEST NOMINMAX WIN32_LEAN_AND_MEAN)
target_include_directories(virtual_desktops_test PRIVATE ${QS_WINDOWS_SRC})
target_link_libraries(virtual_desktops_test PRIVATE Qt6::Gui Qt6::Qml Qt6::Test ole32)
add_test(NAME virtual_desktops COMMAND virtual_desktops_test)

# Exercise the real core runtime-path implementation in separate processes so
# each failure case gets fresh directory state and an isolated working folder.
qs_add_test_executable(runtime_paths_test
	runtime_paths_test.cpp
	${QS_WINDOWS_SRC}/../core/paths.cpp
	${QS_WINDOWS_SRC}/../core/instanceinfo.cpp
	${QS_WINDOWS_SRC}/../core/winfilelock.cpp
)
target_compile_definitions(runtime_paths_test PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(runtime_paths_test PRIVATE Qt6::Core Qt6::Test)
add_test(NAME runtime_paths COMMAND runtime_paths_test)

qs_add_test_executable(lazyloader_test
	lazyloader_test.cpp lazyloader_test_stubs.cpp
	${QS_WINDOWS_SRC}/../core/lazyloader.cpp
	${QS_WINDOWS_SRC}/../core/incubator.cpp
	${QS_WINDOWS_SRC}/../core/reload.hpp
)
target_link_libraries(lazyloader_test PRIVATE Qt6::QuickPrivate Qt6::Qml Qt6::Test)
add_test(NAME lazyloader COMMAND lazyloader_test)
set_tests_properties(lazyloader PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# A console-only copy of the fixture is the daemon child. It never starts qs,
# initializes GUI providers or touches the desktop.
qs_add_test_executable(daemon_handshake_test
	daemon_handshake_test.cpp
	${QS_WINDOWS_SRC}/../launch/daemon_win.cpp
)
target_compile_definitions(daemon_handshake_test PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(daemon_handshake_test PRIVATE Qt6::Core Qt6::Test)
add_test(NAME daemon_handshake COMMAND daemon_handshake_test)
set_tests_properties(daemon_handshake PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

enable_testing()

qs_add_test_executable(tray_hook_test
	tray_hook_test.cpp
	${QS_WINDOWS_SRC}/services/systray/hook.cpp
	${QS_WINDOWS_SRC}/services/systray/tray_reorder.cpp
	${QS_WINDOWS_SRC}/util.cpp
)
target_compile_definitions(tray_hook_test PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN QS_TRAY_HOOK_TEST)
target_link_libraries(tray_hook_test PRIVATE
	Qt6::GuiPrivate Qt6::Qml Qt6::Test user32 shell32 gdi32 advapi32 dwmapi ole32
)
add_test(NAME tray_hook COMMAND tray_hook_test)

# The tray host watches each icon owner's process instead of polling every
# 10 s. The helper owns a window and dies without NIM_DELETE, so the fixture
# sees whether the icon is removed from the process signal alone.
qs_add_test_executable(tray_owner_helper tray_owner_helper.cpp)
target_compile_definitions(tray_owner_helper PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(tray_owner_helper PRIVATE user32)

qs_add_test_executable(tray_host_test
	tray_host_test.cpp
	${QS_WINDOWS_SRC}/services/systray/host.cpp
	${QS_WINDOWS_SRC}/services/systray/item.cpp
	${QS_WINDOWS_SRC}/../core/imageprovider.cpp
)
add_dependencies(tray_host_test tray_owner_helper)
target_compile_definitions(tray_host_test PRIVATE
	QS_TEST QS_TRAY_HOST_TEST NOMINMAX WIN32_LEAN_AND_MEAN
)
target_include_directories(tray_host_test PRIVATE ${QS_WINDOWS_SRC})
target_link_libraries(tray_host_test PRIVATE
	Qt6::Gui Qt6::GuiPrivate Qt6::Quick Qt6::Qml Qt6::Test user32 shell32 version
)
add_test(NAME tray_host COMMAND tray_host_test)
set_tests_properties(tray_host PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# Production manager/window-callback routing with fake platform registration
# and shell services. Never installs a hook or injects desktop input.
qs_add_test_executable(hotkeys_routing_test
	hotkeys_routing_test.cpp
	${QS_WINDOWS_SRC}/hotkeys.hpp
	${QS_WINDOWS_SRC}/input_mask.hpp
	${QS_WINDOWS_SRC}/system/keyboard.hpp
	${QS_WINDOWS_SRC}/input_settle.cpp
	${QS_WINDOWS_SRC}/activation_policy.cpp
	${QS_WINDOWS_SRC}/../core/redact.cpp
)
target_compile_definitions(hotkeys_routing_test PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_include_directories(hotkeys_routing_test PRIVATE ${QS_WINDOWS_SRC})
target_link_libraries(hotkeys_routing_test PRIVATE Qt6::Quick Qt6::Qml Qt6::Test user32)
add_test(NAME hotkeys_routing COMMAND hotkeys_routing_test)

# Target selection, read-only KLID/name resolution and switch result codes.
# The production name source is included with ActivateKeyboardLayout renamed,
# so a regression that activates a layout fails the read-only assertion.
qs_add_test_executable(keyboard_layout_test
	keyboard_layout_test.cpp
	${QS_WINDOWS_SRC}/system/registry_util.cpp
)
target_compile_definitions(keyboard_layout_test PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_include_directories(keyboard_layout_test PRIVATE ${QS_WINDOWS_SRC})
target_link_libraries(keyboard_layout_test PRIVATE
	Qt6::Core Qt6::Test ole32 oleaut32 shlwapi advapi32 dwmapi user32 uuid
)
add_test(NAME keyboard_layout COMMAND keyboard_layout_test)

qs_add_test_executable(keyboard_hook_test
	keyboard_hook_test.cpp
	${QS_WINDOWS_SRC}/input_mask.hpp
)
target_compile_definitions(keyboard_hook_test PRIVATE QS_TEST NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(keyboard_hook_test PRIVATE
	Qt6::Gui Qt6::GuiPrivate Qt6::Qml Qt6::Test user32 advapi32
)
add_test(NAME keyboard_hook COMMAND keyboard_hook_test)

qs_add_test_executable(native_lifetime_test
	native_lifetime_test.cpp
	${QS_WINDOWS_SRC}/services/message_window.cpp
)
target_compile_definitions(native_lifetime_test PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(native_lifetime_test PRIVATE Qt6::Core Qt6::Test user32)
add_test(NAME native_lifetime COMMAND native_lifetime_test)

add_test(NAME bt_pairing_policy COMMAND bt_pairing_policy_test)
qs_add_test_executable(mpris_status_test
	mpris_status_test.cpp
	${QS_WINDOWS_SRC}/services/mpris/backend_status.hpp
)
target_include_directories(mpris_status_test PRIVATE ${QS_WINDOWS_SRC}/services/mpris)
target_link_libraries(mpris_status_test PRIVATE Qt6::Core Qt6::Test)

add_test(NAME mpris_status COMMAND mpris_status_test)

# Notification-mirror consent fixtures. The WinRT listener, app activation and
# window lookup are faked through the ToastListenerSeam / NotificationServer
# test seams, so nothing here prompts, activates an app or removes a real
# Windows notification. The link shims in the test replace the platform
# singletons the notification sources reference.
qs_add_test_executable(notification_mirror_consent_test
	notification_mirror_consent_test.cpp
	${QS_WINDOWS_SRC}/services/notifications/notification.cpp
	${QS_WINDOWS_SRC}/services/notifications/notify_send.cpp
	${QS_WINDOWS_SRC}/services/notifications/server.cpp
	${QS_WINDOWS_SRC}/services/notifications/session_bus_lock.cpp
	${QS_WINDOWS_SRC}/services/notifications/toast_mirror.cpp
	${QS_WINDOWS_SRC}/services/notifications/toast_worker.cpp
	${QS_WINDOWS_SRC}/../core/redact.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/../../core/instanceinfo.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/../../core/model.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/../../core/retainable.cpp
)
# QS_NOTIFICATION_TEST_SEAMS compiles the bus class/mutex override into the
# server sources of this target only. No production target defines it, so a
# release build has no way to redirect where notification content is forwarded.
target_compile_definitions(notification_mirror_consent_test PRIVATE
	NOMINMAX WIN32_LEAN_AND_MEAN QS_NOTIFICATION_TEST_SEAMS
)
target_include_directories(notification_mirror_consent_test PRIVATE ${QS_WINDOWS_SRC})
target_link_libraries(notification_mirror_consent_test PRIVATE
	Qt6::Quick Qt6::Qml Qt6::Gui Qt6::Test windowsapp runtimeobject
)
add_test(NAME notification_mirror_consent COMMAND notification_mirror_consent_test)

qs_add_test_executable(mpris_art_cache_test
	mpris_art_cache_test.cpp
	${QS_WINDOWS_SRC}/services/mpris/art_cache.cpp
	${QS_WINDOWS_SRC}/../core/redact.cpp
)
target_compile_definitions(mpris_art_cache_test PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_include_directories(mpris_art_cache_test PRIVATE ${QS_WINDOWS_SRC}/services/mpris)
target_link_libraries(mpris_art_cache_test PRIVATE Qt6::Gui Qt6::Test windowsapp runtimeobject)
add_test(NAME mpris_art_cache COMMAND mpris_art_cache_test)

# Media app identity is resolved at runtime: a packaged inbox AUMID through
# AppInfo, a desktop executable through its AppsFolder item or version
# resources. The executable AUMID and window fallback cases run the real alias
# lookup against the real desktop entry scan. The fixture only reads shell
# metadata; it never starts a session.
qs_add_test_executable(mpris_app_identity_test
	mpris_app_identity_test.cpp
	${QS_WINDOWS_SRC}/desktopentry_backend.cpp
	${QS_WINDOWS_SRC}/../core/redact.cpp
	${QS_WINDOWS_SRC}/services/mpris/app_identity.cpp
)
target_compile_definitions(mpris_app_identity_test PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_include_directories(mpris_app_identity_test PRIVATE ${QS_WINDOWS_SRC}/services/mpris)
target_link_libraries(mpris_app_identity_test PRIVATE
	Qt6::Core Qt6::Qml Qt6::Test shell32 ole32 windowsapp runtimeobject
)
add_test(NAME mpris_app_identity COMMAND mpris_app_identity_test)
add_test(NAME pw_audio_control COMMAND pw_audio_control_test)
add_test(NAME upower_status COMMAND upower_status_test)
add_test(NAME tiling_observability COMMAND tiling_observability_test)
add_test(NAME tiling_manager COMMAND tiling_manager_test)
add_test(NAME tiling_drag COMMAND tiling_drag_test)
add_test(NAME tiling_reload COMMAND tiling_reload_test)

qs_add_test_executable(transition_trace_test transition_trace_test.cpp ${QS_WINDOWS_SRC}/transition_trace.cpp)
target_compile_definitions(transition_trace_test PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(transition_trace_test PRIVATE Qt6::Core Qt6::Test)
add_test(NAME transition_trace COMMAND transition_trace_test)

# Pure system-surface classification over Windows window metadata. The fixture
# uses the real class and style observations but never needs the live desktop,
# a foreground change or an AppBar registration.
qs_add_test_executable(system_surface_test
	system_surface_test.cpp
	${QS_WINDOWS_SRC}/system_surface.cpp
	${QS_WINDOWS_SRC}/window_selectors.cpp
)
target_compile_definitions(system_surface_test PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_include_directories(system_surface_test PRIVATE ${QS_WINDOWS_SRC})
target_link_libraries(system_surface_test PRIVATE Qt6::Core Qt6::Test user32 dwmapi)
add_test(NAME system_surface COMMAND system_surface_test)

# The foreground observer's raw event stream. The fixture supplies a fake
# tracker and fake window identities, so no hook, foreground change or live
# desktop is needed; the production observer source is still compiled as-is.
qs_add_test_executable(foreground_observer_test
	foreground_observer_test.cpp
	${QS_WINDOWS_SRC}/foreground_observer.cpp
	${QS_WINDOWS_SRC}/foreground_observer.hpp
	${QS_WINDOWS_SRC}/window_tracker.hpp
)
target_compile_definitions(foreground_observer_test PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_include_directories(foreground_observer_test PRIVATE ${QS_WINDOWS_SRC})
target_link_libraries(foreground_observer_test PRIVATE Qt6::Core Qt6::Gui Qt6::Qml Qt6::Test user32)
add_test(NAME foreground_observer COMMAND foreground_observer_test)
set_tests_properties(foreground_observer PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# The activation policy's pure handoff decision and a source guard over the
# activation path. No window, foreground change, input or timer is involved.
qs_add_test_executable(activation_policy_test
	activation_policy_test.cpp
	${QS_WINDOWS_SRC}/activation_policy.cpp
)
target_compile_definitions(activation_policy_test PRIVATE
	NOMINMAX WIN32_LEAN_AND_MEAN
	QS_WINDOWS_SRC="${QS_WINDOWS_SRC}"
)
target_include_directories(activation_policy_test PRIVATE ${QS_WINDOWS_SRC})
target_link_libraries(activation_policy_test PRIVATE Qt6::Core Qt6::Test user32)
add_test(NAME activation_policy COMMAND activation_policy_test)

# Pure pending-open state machine for ABN_FULLSCREENAPP. No HWND, tracker,
# panel or desktop state is needed.
qs_add_test_executable(fullscreen_app_state_test fullscreen_app_state_test.cpp)
target_compile_definitions(fullscreen_app_state_test PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_include_directories(fullscreen_app_state_test PRIVATE ${QS_WINDOWS_SRC})
target_link_libraries(fullscreen_app_state_test PRIVATE Qt6::Core Qt6::Test)
add_test(NAME fullscreen_app_state COMMAND fullscreen_app_state_test)

qs_add_test_executable(panel_topmost_test panel_topmost_test.cpp)
target_include_directories(panel_topmost_test PRIVATE ${QS_WINDOWS_SRC})
target_link_libraries(panel_topmost_test PRIVATE Qt6::Core Qt6::Test)
add_test(NAME panel_topmost COMMAND panel_topmost_test)

qs_add_test_executable(power_profiles_test
	power_profiles_test.cpp
	${QS_WINDOWS_SRC}/services/upower/powerprofiles.cpp
)

target_compile_definitions(power_profiles_test PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_include_directories(power_profiles_test PRIVATE ${QS_WINDOWS_SRC}/services/upower)
target_link_libraries(power_profiles_test PRIVATE Qt6::Qml Qt6::Test)

add_test(NAME power_profiles COMMAND power_profiles_test)

qs_add_test_executable(brightness_route_test
	brightness_route_test.cpp
	${QS_WINDOWS_SRC}/screen_identity.cpp
	${QS_WINDOWS_SRC}/system/brightness.cpp
	${QS_WINDOWS_SRC}/system/gamma.cpp
	${QS_WINDOWS_SRC}/../core/redact.cpp
)
target_compile_definitions(brightness_route_test PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(brightness_route_test PRIVATE
	Qt6::Gui Qt6::Qml Qt6::Test user32 gdi32 dxva2 wbemuuid ole32 oleaut32
)
add_test(NAME brightness_route COMMAND brightness_route_test)

# Two synthetic monitors with the same vendor/model label. Brightness and gamma
# route by the persistent target identity, never by the label. The fixture
# injects the native brightness backend and gamma calls.
qs_add_test_executable(display_identity_test
	display_identity_test.cpp
	${QS_WINDOWS_SRC}/screen_identity.cpp
	${QS_WINDOWS_SRC}/system/brightness.cpp
	${QS_WINDOWS_SRC}/system/gamma.cpp
	${QS_WINDOWS_SRC}/../core/qmlscreen.cpp
	${QS_WINDOWS_SRC}/../core/redact.cpp
)
target_compile_definitions(display_identity_test PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(display_identity_test PRIVATE
	Qt6::Gui Qt6::GuiPrivate Qt6::Qml Qt6::Test user32 gdi32 dxva2 wbemuuid ole32 oleaut32
)
add_test(NAME display_identity COMMAND display_identity_test)
set_tests_properties(display_identity PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# The real gamma state machine runs with injected Create/Get/Set/Delete calls.
# The fixture never instantiates the native brightness backend or a QML shell.

# The #220 handback and the #214 visibility rule against temporary windows, in
# three processes: this test is the shell, a copy owns the host window and
# another owns the wallpaper window and restacks, hides and shows it, so
# isOwnProcessWindow() sees a real external owner and the host-scoped hooks are
# as blind as in production. Nothing touches Explorer's Progman/WorkerW.
qs_add_test_executable(desktop_zorder_test
	desktop_zorder_test.cpp
	${QS_WINDOWS_SRC}/desktop_host.cpp
	${QS_WINDOWS_SRC}/desktop_host.hpp
	${QS_WINDOWS_SRC}/activation_policy.cpp
	${QS_WINDOWS_SRC}/util.cpp
	${QS_WINDOWS_SRC}/appbar.hpp
)
target_compile_definitions(desktop_zorder_test PRIVATE QS_TEST NOMINMAX WIN32_LEAN_AND_MEAN)
target_include_directories(desktop_zorder_test PRIVATE ${QS_WINDOWS_SRC})
target_link_libraries(desktop_zorder_test PRIVATE
	Qt6::Gui Qt6::GuiPrivate Qt6::Qml Qt6::Test user32 advapi32 dwmapi ole32
)
add_test(NAME desktop_zorder COMMAND desktop_zorder_test)
set_tests_properties(desktop_zorder PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# Owns a private desktop and never switches to it. No input-desktop windows,
# Explorer lookup or desktop-wide hooks are used.
qs_add_test_executable(desktop_underlay_test
	desktop_underlay_test.cpp
	${QS_WINDOWS_SRC}/desktop_host.cpp
	${QS_WINDOWS_SRC}/desktop_host.hpp
	${QS_WINDOWS_SRC}/activation_policy.cpp
	${QS_WINDOWS_SRC}/util.cpp
	${QS_WINDOWS_SRC}/appbar.hpp
)
target_compile_definitions(desktop_underlay_test PRIVATE QS_TEST NOMINMAX WIN32_LEAN_AND_MEAN)
target_include_directories(desktop_underlay_test PRIVATE ${QS_WINDOWS_SRC})
target_link_libraries(desktop_underlay_test PRIVATE
	Qt6::Gui Qt6::GuiPrivate Qt6::Qml Qt6::Test user32 gdi32 advapi32 dwmapi ole32
)
add_test(NAME desktop_underlay COMMAND desktop_underlay_test)
set_tests_properties(desktop_underlay PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# #214 P2: a hidden WinPanelWindow keeps its QQuickWindow and HWND
# (deleteOnInvisible is false), and a later host replacement re-embeds it.
# Re-embedding must not reveal the hidden panel. The fixture instantiates the
# production panel graph, so it needs a native HWND and runs on the Windows QPA
# instead of the offscreen one. The stubs keep the QML engine, desktop accessor
# and drag paths out of the fixture. Nothing touches Explorer's desktop.
set(QS_PANEL_REEMBED_TEST_SOURCES
	panel_hidden_reembed_test.cpp
	panel_hidden_reembed_test_stubs.cpp
	${QS_WINDOWS_SRC}/panel_window.cpp
	${QS_WINDOWS_SRC}/panel_window.hpp
	${QS_WINDOWS_SRC}/appbar.cpp
	${QS_WINDOWS_SRC}/appbar.hpp
	${QS_WINDOWS_SRC}/blur.cpp
	${QS_WINDOWS_SRC}/blur.hpp
	${QS_WINDOWS_SRC}/blur_shapes.cpp
	${QS_WINDOWS_SRC}/input_mask.cpp
	${QS_WINDOWS_SRC}/input_mask.hpp
	${QS_WINDOWS_SRC}/panel_animation.cpp
	${QS_WINDOWS_SRC}/panel_animation.hpp
	${QS_WINDOWS_SRC}/panel_effects.cpp
	${QS_WINDOWS_SRC}/panel_effects.hpp
	${QS_WINDOWS_SRC}/panel_topmost.hpp
	${QS_WINDOWS_SRC}/layer_rules.cpp
	${QS_WINDOWS_SRC}/fullscreen_occluders.cpp
	${QS_WINDOWS_SRC}/screen_identity.cpp
	${QS_WINDOWS_SRC}/screen_identity.hpp
	${QS_WINDOWS_SRC}/startup.cpp
	${QS_WINDOWS_SRC}/system_surface.cpp
	${QS_WINDOWS_SRC}/system_surface.hpp
	${QS_WINDOWS_SRC}/transition_trace.cpp
	${QS_WINDOWS_SRC}/virtual_desktops.cpp
	${QS_WINDOWS_SRC}/virtual_desktops.hpp
	${QS_WINDOWS_SRC}/window_tracker.cpp
	${QS_WINDOWS_SRC}/window_tracker.hpp
	${QS_WINDOWS_SRC}/window_selectors.cpp
	${QS_WINDOWS_SRC}/window_selectors.hpp
	${QS_WINDOWS_SRC}/native_desktop_accessor.cpp
	${QS_WINDOWS_SRC}/native_desktop_accessor.hpp
	${QS_WINDOWS_SRC}/accessor_state.hpp
	${QS_WINDOWS_SRC}/services/message_window.cpp
	${QS_WINDOWS_SRC}/services/message_window.hpp
	${QS_WINDOWS_SRC}/desktop_host.cpp
	${QS_WINDOWS_SRC}/desktop_host.hpp
	${QS_WINDOWS_SRC}/activation_policy.cpp
	${QS_WINDOWS_SRC}/util.cpp
	${QS_WINDOWS_SRC}/../core/reload.hpp
	${QS_WINDOWS_SRC}/../core/qmlscreen.cpp
	${QS_WINDOWS_SRC}/../core/qmlscreen.hpp
	${QS_WINDOWS_SRC}/../core/region.cpp
	${QS_WINDOWS_SRC}/../core/types.cpp
	${QS_WINDOWS_SRC}/../core/redact.cpp
	${QS_WINDOWS_SRC}/../window/proxywindow.cpp
	${QS_WINDOWS_SRC}/../window/proxywindow.hpp
	${QS_WINDOWS_SRC}/../window/windowinterface.cpp
	${QS_WINDOWS_SRC}/../window/windowinterface.hpp
	${QS_WINDOWS_SRC}/../window/panelinterface.cpp
	${QS_WINDOWS_SRC}/../window/panelinterface.hpp
)
qs_add_test_executable(panel_hidden_reembed_test ${QS_PANEL_REEMBED_TEST_SOURCES})
target_compile_definitions(panel_hidden_reembed_test PRIVATE QS_TEST NOMINMAX WIN32_LEAN_AND_MEAN)
target_include_directories(panel_hidden_reembed_test PRIVATE
	${QS_WINDOWS_SRC}
	${QS_WINDOWS_SRC}/..
	${QS_WINDOWS_SRC}/../core
	${QS_WINDOWS_SRC}/../window
)
target_link_libraries(panel_hidden_reembed_test PRIVATE
	Qt6::Gui Qt6::GuiPrivate Qt6::Qml Qt6::Quick Qt6::QuickPrivate Qt6::Test
	user32 advapi32 dwmapi ole32 oleaut32 shell32 shlwapi gdi32
	windowsapp runtimeobject CoreMessaging
)
add_test(NAME panel_hidden_reembed COMMAND panel_hidden_reembed_test)
set_tests_properties(panel_hidden_reembed PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=windows")

qs_add_test_executable(native_desktop_recovery_test
	native_desktop_recovery_test.cpp
	${QS_WINDOWS_SRC}/native_desktop_accessor.cpp
)
target_compile_definitions(native_desktop_recovery_test PRIVATE QS_TEST NOMINMAX WIN32_LEAN_AND_MEAN)
target_include_directories(native_desktop_recovery_test PRIVATE ${QS_WINDOWS_SRC})
target_link_libraries(native_desktop_recovery_test PRIVATE Qt6::Gui Qt6::Qml Qt6::Test ole32)
add_test(NAME native_desktop_recovery COMMAND native_desktop_recovery_test)

qs_add_test_executable(pw_session_worker_test
	pw_session_worker_test.cpp
	${QS_WINDOWS_SRC}/services/pipewire/pw_session_worker.cpp
	${QS_WINDOWS_SRC}/services/pipewire/pw_node.cpp
	${QS_WINDOWS_SRC}/services/pipewire/pw_types.cpp
	${QS_WINDOWS_SRC}/../core/redact.cpp
)
target_compile_definitions(pw_session_worker_test PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_include_directories(pw_session_worker_test PRIVATE ${QS_WINDOWS_SRC}/services/pipewire)
target_link_libraries(pw_session_worker_test PRIVATE Qt6::Qml Qt6::Test ole32)
add_test(NAME pw_session_worker COMMAND pw_session_worker_test)

qs_add_test_executable(idle_request_test idle_request_test.cpp)
target_compile_definitions(idle_request_test PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(idle_request_test PRIVATE Qt6::Core Qt6::Test)
add_test(NAME idle_request COMMAND idle_request_test)

# The key-release gate behind `displays off`: settles only on the last release,
# cancels on new input, and ignores key repeat.
qs_add_test_executable(input_settle_test
	input_settle_test.cpp
	${QS_WINDOWS_SRC}/input_settle.cpp
)
target_compile_definitions(input_settle_test PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_include_directories(input_settle_test PRIVATE ${QS_WINDOWS_SRC})
target_link_libraries(input_settle_test PRIVATE Qt6::Core Qt6::Test)
add_test(NAME input_settle COMMAND input_settle_test)

# Super-drag restore/placement waits: geometry decides completion, the deadline
# only reports failure.
qs_add_test_executable(super_drag_wait_test super_drag_wait_test.cpp)
target_compile_definitions(super_drag_wait_test PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_include_directories(super_drag_wait_test PRIVATE ${QS_WINDOWS_SRC})
target_link_libraries(super_drag_wait_test PRIVATE Qt6::Core Qt6::Test)
add_test(NAME super_drag_wait COMMAND super_drag_wait_test)

# WLAN interface routing: one explicit selection, privileged by connection
# state, kept while it stays enumerated, and every notification filtered by its
# GUID. Also the completed-refresh snapshot: the real Network state and networks
# model, with the WLAN and connectivity backends stubbed and never started.
qs_add_test_executable(network_wifi_route_test
	network_wifi_route_test.cpp
	network_wifi_stubs.cpp
	${QS_WINDOWS_SRC}/system/network.cpp
	${QS_WINDOWS_SRC}/system/network.hpp
	${QS_WINDOWS_SRC}/system/network_wifi.hpp
	${QS_WINDOWS_SRC}/system/network_connectivity.hpp
	${QS_WINDOWS_SRC}/system/network_wifi_item.cpp
	${QS_WINDOWS_SRC}/system/network_wifi_item.hpp
	${QS_WINDOWS_SRC}/../core/model.cpp
)
target_compile_definitions(network_wifi_route_test PRIVATE
	QS_TEST NOMINMAX WIN32_LEAN_AND_MEAN
)
target_include_directories(network_wifi_route_test PRIVATE ${QS_WINDOWS_SRC})
target_link_libraries(network_wifi_route_test PRIVATE Qt6::Core Qt6::Qml Qt6::Test)
add_test(NAME network_wifi_route COMMAND network_wifi_route_test)

qs_add_test_executable(fullscreen_ownership_test fullscreen_ownership_test.cpp)
target_compile_definitions(fullscreen_ownership_test PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(fullscreen_ownership_test PRIVATE Qt6::Core Qt6::Test user32)
add_test(NAME fullscreen_ownership COMMAND fullscreen_ownership_test)

qs_add_test_executable(display_transaction_test
	display_transaction_test.cpp
	display_transaction_stubs.cpp
	${QS_WINDOWS_SRC}/system/displays.cpp
	${QS_WINDOWS_SRC}/system/displays.hpp
)
target_compile_definitions(display_transaction_test PRIVATE
	QS_DISPLAYS_TEST NOMINMAX WIN32_LEAN_AND_MEAN
)
target_link_libraries(display_transaction_test PRIVATE Qt6::Gui Qt6::Qml Qt6::Test user32)
add_test(NAME display_transaction COMMAND display_transaction_test)

qs_add_test_executable(clipboard_capture_test
	clipboard_capture_test.cpp
	clipboard_capture_test_stubs.cpp
	${QS_WINDOWS_SRC}/system/clipboard.cpp
	${QS_WINDOWS_SRC}/system/clipboard.hpp
	${QS_WINDOWS_SRC}/../core/redact.cpp
)
target_compile_definitions(clipboard_capture_test PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(clipboard_capture_test PRIVATE Qt6::Gui Qt6::Qml Qt6::Test user32 gdi32)
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/clipboard-fixture-data")
add_test(NAME clipboard_capture COMMAND clipboard_capture_test
	-o "${CMAKE_CURRENT_BINARY_DIR}/clipboard-qt.xml,xml" -o -,txt
)
set_tests_properties(clipboard_capture PROPERTIES ENVIRONMENT
	"QS_CLIPBOARD_FIXTURE_ROOT=${CMAKE_CURRENT_BINARY_DIR}/clipboard-fixture-data;QT_QPA_PLATFORM=offscreen"
)

qs_add_test_executable(capture_dimensions_test capture_dimensions_test.cpp)
target_compile_definitions(capture_dimensions_test PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_include_directories(capture_dimensions_test PRIVATE ${QS_WINDOWS_SRC})
target_link_libraries(capture_dimensions_test PRIVATE Qt6::Gui Qt6::Test windowsapp ole32)
add_test(NAME capture_dimensions COMMAND capture_dimensions_test)
set_tests_properties(capture_dimensions PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

qs_add_test_executable(erm625_native_desktop_facade_test
	erm625_native_desktop_facade_test.cpp
	${QS_WINDOWS_SRC}/virtual_desktops.cpp
	${QS_WINDOWS_SRC}/virtual_desktops.hpp
	${QS_WINDOWS_SRC}/native_desktop_accessor.cpp
	${QS_WINDOWS_SRC}/accessor_state.hpp
	${QS_WINDOWS_SRC}/services/message_window.cpp
	${QS_WINDOWS_SRC}/../core/redact.cpp
)
target_compile_definitions(erm625_native_desktop_facade_test PRIVATE QS_TEST NOMINMAX WIN32_LEAN_AND_MEAN)
target_include_directories(erm625_native_desktop_facade_test PRIVATE ${QS_WINDOWS_SRC})
target_link_libraries(erm625_native_desktop_facade_test PRIVATE Qt6::Gui Qt6::Qml Qt6::Test ole32)
add_test(NAME erm625_native_desktop_facade COMMAND erm625_native_desktop_facade_test)
set_tests_properties(erm625_native_desktop_facade PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

qs_add_test_executable(network_wifi_identity_test
	network_wifi_identity_test.cpp
	network_wifi_native_stubs.cpp
	network_wifi_stubs.cpp
	${QS_WINDOWS_SRC}/system/network.cpp
	${QS_WINDOWS_SRC}/system/network.hpp
	${QS_WINDOWS_SRC}/system/network_wifi.cpp
	${QS_WINDOWS_SRC}/system/network_wifi.hpp
	${QS_WINDOWS_SRC}/system/network_connectivity.hpp
	${QS_WINDOWS_SRC}/system/network_wifi_item.cpp
	${QS_WINDOWS_SRC}/system/network_wifi_item.hpp
	${QS_WINDOWS_SRC}/../core/model.cpp
)
target_compile_definitions(network_wifi_identity_test PRIVATE
	QS_TEST QS_WIFI_NATIVE_FIXTURE NOMINMAX WIN32_LEAN_AND_MEAN
)
target_include_directories(network_wifi_identity_test PRIVATE ${QS_WINDOWS_SRC})
target_link_libraries(network_wifi_identity_test PRIVATE Qt6::Core Qt6::Qml Qt6::Test shell32)
add_test(NAME network_wifi_identity COMMAND network_wifi_identity_test)

# The capture exclusion decision with fake Win32 calls. No window and no desktop.
qs_add_test_executable(capture_affinity_test capture_affinity_test.cpp)
target_compile_definitions(capture_affinity_test PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_include_directories(capture_affinity_test PRIVATE ${QS_WINDOWS_SRC})
target_link_libraries(capture_affinity_test PRIVATE Qt6::Core Qt6::Test user32)
add_test(NAME capture_affinity COMMAND capture_affinity_test)

# The window stack snapshot's classification, frame mapping and QML conversion.
# It never enumerates a desktop.
qs_add_test_executable(window_stack_test
	window_stack_test.cpp
	${QS_WINDOWS_SRC}/window_stack.cpp
	${QS_WINDOWS_SRC}/screen_identity.cpp
	${QS_WINDOWS_SRC}/../core/redact.cpp
	${QS_WINDOWS_SRC}/system_surface.cpp
	${QS_WINDOWS_SRC}/window_selectors.cpp
	${QS_WINDOWS_SRC}/activation_policy.cpp
	${QS_WINDOWS_SRC}/util.cpp
)
target_compile_definitions(window_stack_test PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_include_directories(window_stack_test PRIVATE ${QS_WINDOWS_SRC})
target_link_libraries(window_stack_test PRIVATE
	Qt6::Gui Qt6::GuiPrivate Qt6::Qml Qt6::Test user32 advapi32 dwmapi ole32
)
add_test(NAME window_stack COMMAND window_stack_test)
set_tests_properties(window_stack PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# Capture exclusion and the window stack against real windows on a private
# desktop that is never switched to: affinity round trips for top-level, layered
# and child windows, and the enumeration order, restacking and exclusions.
qs_add_test_executable(capture_native_test
	capture_native_test.cpp
	${QS_WINDOWS_SRC}/window_stack.cpp
	${QS_WINDOWS_SRC}/screen_identity.cpp
	${QS_WINDOWS_SRC}/../core/redact.cpp
	${QS_WINDOWS_SRC}/system_surface.cpp
	${QS_WINDOWS_SRC}/window_selectors.cpp
	${QS_WINDOWS_SRC}/activation_policy.cpp
	${QS_WINDOWS_SRC}/util.cpp
)
target_compile_definitions(capture_native_test PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_include_directories(capture_native_test PRIVATE ${QS_WINDOWS_SRC})
target_link_libraries(capture_native_test PRIVATE
	Qt6::Gui Qt6::GuiPrivate Qt6::Qml Qt6::Test user32 gdi32 advapi32 dwmapi ole32
)
add_test(NAME capture_native COMMAND capture_native_test)

# Runtime tiers. Every registered fixture belongs to exactly one tier, and
# configuration fails on an unclassified or doubly classified fixture.
#   portable: fakes and offscreen Qt only; required in hosted CI.
#   native:   hidden or message-only windows, helper processes or a private
#             desktop; no visible input-desktop windows.
#   desktop:  visible native windows on the input desktop; run by hand with
#             the shell stopped.
#   gpu:      real D3D11 devices or RHI rendering.
#   machine:  reads installed-app metadata from this machine.
set(QS_TESTS_PORTABLE
	wallpaper_target screen_identity hyprland_monitor panel_animation
	screenshot_jobs fs_utils ipc_limits redaction log_scrub log_redaction_guard
	tiling_layout dispatcher virtual_desktops hotkeys_routing keyboard_hook
	bt_pairing_policy mpris_status mpris_art_cache pw_audio_control
	upower_status tiling_observability tiling_manager tiling_drag tiling_reload
	transition_trace foreground_observer activation_policy fullscreen_app_state
	panel_topmost power_profiles brightness_route display_identity
	native_desktop_recovery pw_session_worker idle_request input_settle
	super_drag_wait network_wifi_route runtime_paths lazyloader
	fullscreen_ownership display_transaction clipboard_capture capture_dimensions
	erm625_native_desktop_facade network_wifi_identity capture_affinity window_stack
)
set(QS_TESTS_NATIVE
	tray_hook tray_host keyboard_layout native_lifetime
	notification_mirror_consent desktop_underlay capture_native daemon_handshake
)
set(QS_TESTS_DESKTOP system_surface desktop_zorder panel_hidden_reembed)
set(QS_TESTS_GPU capture_format capture_device video_node)
set(QS_TESTS_MACHINE mpris_app_identity)

get_property(QS_REGISTERED_TESTS DIRECTORY PROPERTY TESTS)
foreach(test IN LISTS QS_REGISTERED_TESTS)
	set(tiers "")
	foreach(tier PORTABLE NATIVE DESKTOP GPU MACHINE)
		if(test IN_LIST QS_TESTS_${tier})
			string(TOLOWER ${tier} label)
			list(APPEND tiers ${label})
		endif()
	endforeach()
	list(LENGTH tiers count)
	if(NOT count EQUAL 1)
		message(FATAL_ERROR "Fixture '${test}' needs exactly one runtime tier, has: '${tiers}'")
	endif()
	set_tests_properties(${test} PROPERTIES LABELS ${tiers} TIMEOUT 120)
	# Without a console Qt Test sends its log to OutputDebugString, so CTest
	# would capture no assertion or skip text.
	set_property(TEST ${test} APPEND PROPERTY ENVIRONMENT QT_FORCE_STDERR_LOGGING=1)
endforeach()
foreach(tier PORTABLE NATIVE DESKTOP GPU MACHINE)
	foreach(test IN LISTS QS_TESTS_${tier})
		if(NOT test IN_LIST QS_REGISTERED_TESTS)
			message(FATAL_ERROR "Tier list names unregistered fixture '${test}'")
		endif()
	endforeach()
endforeach()
