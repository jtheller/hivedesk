# Pinned upstream dispatcher, used ONLY by HiveDeskMediaProbe.
set(vpl_windows main mfx_critical_section mfx_dispatcher mfx_dispatcher_log
    mfx_driver_store_loader mfx_dxva2_device mfx_function_table
    mfx_library_iterator mfx_load_dll mfx_win_reg_key)
set(vpl_common mfx_dispatcher_vpl mfx_dispatcher_vpl_loader mfx_dispatcher_vpl_config
    mfx_dispatcher_vpl_lowlatency mfx_dispatcher_vpl_log mfx_dispatcher_vpl_msdk)
set(vpl_sources)
foreach(source IN LISTS vpl_windows)
  list(APPEND vpl_sources dispatcher/src/windows/${source}.cpp)
endforeach()
foreach(source IN LISTS vpl_common)
  list(APPEND vpl_sources dispatcher/src/${source}.cpp)
endforeach()
list(APPEND vpl_sources dispatcher/src/mfx_config_interface/mfx_config_interface.cpp
  dispatcher/src/mfx_config_interface/mfx_config_interface_string_api.cpp)
add_library(hive_probe_vpl STATIC ${vpl_sources})
target_include_directories(hive_probe_vpl PUBLIC api PRIVATE dispatcher)
target_compile_definitions(hive_probe_vpl PRIVATE MFX_DEPRECATED_OFF)
target_compile_options(hive_probe_vpl PRIVATE /FIobjbase.h)
target_link_libraries(hive_probe_vpl PRIVATE dxgi cfgmgr32 advapi32)
