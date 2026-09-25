include(${PROJECT_SOURCE_DIR}/cmake/Catch2.cmake)
include(${PROJECT_SOURCE_DIR}/cmake/AfarWarnings.cmake)

add_executable(test_smoke test_smoke.cpp)
target_link_libraries(test_smoke PRIVATE Catch2::Catch2WithMain)
afar_enable_warnings(test_smoke)
add_test(NAME smoke COMMAND test_smoke)

# TEST-002 / AT-02: валидация run-config и attenuator CSV (без железа)
add_executable(test_run_config test_run_config.cpp)
target_link_libraries(test_run_config PRIVATE
    Catch2::Catch2WithMain
    afar_data
    afar_core
)
file(TO_CMAKE_PATH "${PROJECT_SOURCE_DIR}/examples" _afar_examples_dir)
target_compile_definitions(test_run_config PRIVATE
    AFAR_EXAMPLES_DIR="${_afar_examples_dir}"
)
unset(_afar_examples_dir)
afar_enable_warnings(test_run_config)
add_test(NAME run_config COMMAND test_run_config)

add_executable(test_lut_math test_lut_math.cpp)
target_link_libraries(test_lut_math PRIVATE Catch2::Catch2WithMain afar_cal)
afar_enable_warnings(test_lut_math)
add_test(NAME lut_math COMMAND test_lut_math)

add_executable(test_quality_gates test_quality_gates.cpp)
target_link_libraries(test_quality_gates PRIVATE Catch2::Catch2WithMain afar_cal afar_qc)
afar_enable_warnings(test_quality_gates)
add_test(NAME quality_gates COMMAND test_quality_gates)

add_executable(test_simulators test_simulators.cpp)
target_link_libraries(test_simulators PRIVATE Catch2::Catch2WithMain afar_sim afar_dut)
afar_enable_warnings(test_simulators)
add_test(NAME simulators COMMAND test_simulators)

add_executable(test_hdf5_store test_hdf5_store.cpp)
target_link_libraries(test_hdf5_store PRIVATE Catch2::Catch2WithMain afar_data)
target_include_directories(test_hdf5_store PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
afar_enable_warnings(test_hdf5_store)
add_test(NAME hdf5_store COMMAND test_hdf5_store)

add_executable(test_probe_run test_probe_run.cpp)
target_link_libraries(test_probe_run PRIVATE
    Catch2::Catch2WithMain
    afar_measure
    afar_sim
    afar_dut
)
target_include_directories(test_probe_run PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
afar_enable_warnings(test_probe_run)
add_test(NAME probe_run COMMAND test_probe_run)

add_executable(test_pause_resume test_pause_resume.cpp)
target_link_libraries(test_pause_resume PRIVATE
    Catch2::Catch2WithMain
    afar_measure
    afar_sim
    afar_dut
)
target_include_directories(test_pause_resume PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
afar_enable_warnings(test_pause_resume)
add_test(NAME pause_resume COMMAND test_pause_resume)

add_executable(test_crash_recovery test_crash_recovery.cpp)
target_link_libraries(test_crash_recovery PRIVATE
    Catch2::Catch2WithMain
    afar_measure
    afar_sim
    afar_dut
)
target_include_directories(test_crash_recovery PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
afar_enable_warnings(test_crash_recovery)
add_test(NAME crash_recovery COMMAND test_crash_recovery)

add_executable(test_faults test_faults.cpp)
target_link_libraries(test_faults PRIVATE
    Catch2::Catch2WithMain
    afar_measure
    afar_sim
    afar_dut
)
target_include_directories(test_faults PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
afar_enable_warnings(test_faults)
add_test(NAME faults COMMAND test_faults)

# TEST-010 / AT-11: экспорт Parquet+PDF+manifest и контрольные суммы
add_executable(test_export_manifest test_export_manifest.cpp)
target_link_libraries(test_export_manifest PRIVATE
    Catch2::Catch2WithMain
    afar_report
    afar_measure
    afar_sim
    afar_dut
)
target_include_directories(test_export_manifest PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
afar_enable_warnings(test_export_manifest)
add_test(NAME export_manifest COMMAND test_export_manifest)

# TEST-011 / AT-01: драйвер C2220 против локального SCPI TCP-стаба
add_executable(test_c2220_driver test_c2220_driver.cpp)
target_link_libraries(test_c2220_driver PRIVATE
    Catch2::Catch2WithMain
    afar_vna
)
afar_enable_warnings(test_c2220_driver)
add_test(NAME c2220_driver COMMAND test_c2220_driver)

# TEST-012 / AT-04: компактная сетка по умолчанию; полный 65536 при AFAR_RUN_AT04=1 + [full]
add_executable(test_full_sim_run test_full_sim_run.cpp)
target_link_libraries(test_full_sim_run PRIVATE
    Catch2::Catch2WithMain
    afar_measure
    afar_sim
    afar_dut
)
target_include_directories(test_full_sim_run PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
afar_enable_warnings(test_full_sim_run)
add_test(NAME full_sim_run COMMAND test_full_sim_run)
set_tests_properties(full_sim_run PROPERTIES TIMEOUT 120)

# CAL-005 / NFR-03: замер LUT (probe по умолчанию; полный объём — [full] + AFAR_RUN_AT04=1)
add_executable(test_lut_perf test_lut_perf.cpp)
target_link_libraries(test_lut_perf PRIVATE
    Catch2::Catch2WithMain
    afar_report
    afar_measure
    afar_sim
    afar_dut
)
target_include_directories(test_lut_perf PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
afar_enable_warnings(test_lut_perf)
add_test(NAME lut_perf COMMAND test_lut_perf)
set_tests_properties(lut_perf PROPERTIES TIMEOUT 120)

find_package(Qt6 REQUIRED COMPONENTS Widgets)
add_executable(test_ui_interactions
    test_ui_interactions.cpp
    ${PROJECT_SOURCE_DIR}/src/ui/ConnectionBar.cpp
    ${PROJECT_SOURCE_DIR}/src/ui/ConnectionBar.h
    ${PROJECT_SOURCE_DIR}/src/ui/S21PlotWidget.cpp
    ${PROJECT_SOURCE_DIR}/src/ui/S21PlotWidget.h
    ${PROJECT_SOURCE_DIR}/src/ui/MeasureTab.cpp
    ${PROJECT_SOURCE_DIR}/src/ui/MeasureTab.h
)
target_include_directories(test_ui_interactions PRIVATE ${PROJECT_SOURCE_DIR}/src/ui)
target_link_libraries(test_ui_interactions PRIVATE Catch2::Catch2WithMain Qt6::Widgets afar_cal)
set_target_properties(test_ui_interactions PROPERTIES AUTOMOC ON)
afar_enable_warnings(test_ui_interactions)
add_test(NAME ui_interactions COMMAND test_ui_interactions)
set_tests_properties(ui_interactions PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
