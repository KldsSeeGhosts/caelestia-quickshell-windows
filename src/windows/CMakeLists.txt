qt_add_library(quickshell-windows STATIC
	util.cpp
	screen_identity.cpp
	fullscreen_occluders.cpp
	startup.cpp
	appbar.cpp
	input_mask.cpp
	panel_window.cpp
	panel_animation.cpp
	panel_effects.cpp
	layer_rules.cpp
	blur.cpp
	blur_shapes.cpp
	keyboard_hook.cpp
	input_settle.cpp
	hotkeys.cpp
	focus_grab.cpp
	accessor_state.hpp
	virtual_desktops.cpp
	native_desktop_accessor.cpp
	window_tracker.cpp
	window_stack.cpp
	foreground_observer.cpp
	foreground_observer.hpp
	foreground_policy.hpp
	activation_policy.hpp
	activation_policy.cpp
	system_surface.cpp
	window_selectors.cpp
	window_surfaces.cpp
	transition_trace.cpp
	tiling_layout.cpp
	tiling_persistence.cpp
	tiling_scope.cpp
	tiling.cpp
	window_decorations.cpp
	window_manager.cpp
	super_drag.cpp
	taskbar.cpp
	capture.cpp
	capture_device.cpp
	capture_format.cpp
	capture_texture.cpp
	desktop_host.cpp
	desktop_drop.cpp
	video_wallpaper.cpp
	desktopentry_backend.cpp
	appicon.cpp
	system/session.cpp
	system/system_stats.cpp
	system/system_monitor.cpp
	system/brightness.cpp
	system/keyboard.cpp
	system/keyboard_layout_name.cpp
	system/clipboard.cpp
	system/credentials.cpp
	system/input.cpp
	system/input_settings.cpp
	system/night_light.cpp
	system/gamma.cpp
	system/screenshot.cpp
	system/screenshot_jobs.cpp
	system/notification_settings.cpp
	system/account_age.cpp
	system/idle_power.cpp
	system/time_zones.cpp
	system/network.cpp
	system/network_wifi.cpp
	system/network_wifi_item.cpp
	system/network_connectivity.cpp
	system/wallpaper.cpp
	system/fs_utils.cpp
	system/equalizer_apo.cpp
	system/file_index.cpp
	system/terminal_colors.cpp
	system/terminal_colors_math.cpp
	system/registry_util.cpp
	system/shell_link.cpp
	system/webapps.cpp
	system/autostart.cpp
	system/app_actions.cpp
	system/calendar_ics.cpp
	system/calendar_events.cpp
	system/displays.cpp
	image/image_tools.cpp
	image/image_tools_backend.cpp
	image/image_tools_worker.cpp
	image/region_finder.cpp
	image/thumbnailer.cpp
	system/ocr.cpp
	system/ocr_backend.cpp
	system/ocr_worker.cpp
	system/screen_recorder.cpp
	recorder/recording.cpp
	recorder/region_capture.cpp
	recorder/mp4_writer.cpp
	recorder/audio_capture.cpp
	recorder/audio_dsp.cpp
)

qt_add_executable(brightness-probe MANUAL_FINALIZATION
	tools/brightness_probe.cpp
	screen_identity.cpp
	system/brightness.cpp
	system/gamma.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/../core/redact.cpp
)
target_compile_definitions(brightness-probe PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(brightness-probe PRIVATE Qt6::Gui Qt6::Qml user32 gdi32 dxva2 wbemuuid ole32 oleaut32)
set_target_properties(brightness-probe PROPERTIES QT_QML_MODULE_NO_IMPORT_SCAN TRUE)
qt_finalize_target(brightness-probe)

qt_add_qml_module(quickshell-windows
	URI Quickshell.Windows
	VERSION 0.1
	DEPENDENCIES QtQuick
)

qs_add_module_deps_light(quickshell-windows Quickshell)

install_qml_module(quickshell-windows)

qt_add_resources(quickshell-windows "windows-panel-effects"
	PREFIX "/Quickshell/Windows"
	FILES shaders/panel_effect.frag.qsb
)

set(QS_AUTOSTART_NAME "illogical-impulse" CACHE STRING "Run key value and scheduled task prefix Autostart uses to start the shell itself")
set(QS_AUTOSTART_DESCRIPTION "Starts illogical-impulse (ii-windows) when you sign in." CACHE STRING "Description of the scheduled task that starts the shell itself")
set(QS_AUTOSTART_ARGS "-n -c ii" CACHE STRING "Arguments the shell's own startup entry passes to qsw.exe (empty: the config that turned it on)")
configure_file(system/autostart_config.hpp.in "${CMAKE_CURRENT_BINARY_DIR}/autostart_config.hpp" @ONLY)

target_include_directories(quickshell-windows PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/system
	${CMAKE_CURRENT_SOURCE_DIR}/image
	${CMAKE_CURRENT_BINARY_DIR}
)

add_subdirectory(wayland)
add_subdirectory(services)
add_subdirectory(hyprland)
add_subdirectory(bluetooth)
add_subdirectory(third_party/material_color)

if (CRASH_HANDLER)
	add_subdirectory(crash)
endif()

add_library(quickshell-windows-init OBJECT init.cpp animation_timing.cpp)

target_link_libraries(quickshell-windows PRIVATE
	Qt::Quick
	Qt::QuickPrivate
	Qt::Network
	quickshell-windows-services
	user32
	shell32
	shlwapi
	dwmapi
	gdi32
	advapi32
	powrprof
	winmm
	dxva2
	pdh
	winhttp
	wbemuuid
	ole32
	oleaut32
	uuid
	windowsapp
	runtimeobject
	d3d11
	dxgi
	wlanapi
	iphlpapi
	netapi32
	CoreMessaging
	material-color
	mfplat
	mfreadwrite
	mfuuid
	delayimp
)
target_link_libraries(quickshell-windows-init PRIVATE Qt::Quick Qt::QuickPrivate Qt::Qml)
set_source_files_properties(animation_timing.cpp PROPERTIES COMPILE_DEFINITIONS QT_STATICPLUGIN)

qs_module_pch(quickshell-windows SET large)

target_link_libraries(quickshell PRIVATE quickshell-windowsplugin quickshell-windows-init)

set(QS_WINDOWS_DELAYLOAD
	mfplat
	mfreadwrite
	dxva2
	pdh
	winhttp
	winmm
	CoreMessaging
	wlanapi
	iphlpapi
	netapi32
	version
	powrprof
	d3d11
	dxgi
)

foreach(exe quickshell quickshell-gui)
	if (TARGET ${exe})
		foreach(dll ${QS_WINDOWS_DELAYLOAD})
			target_link_options(${exe} PRIVATE /DELAYLOAD:${dll}.dll)
		endforeach()
	endif()
endforeach()
