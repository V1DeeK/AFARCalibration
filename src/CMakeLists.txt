function(afar_add_interface_module stem)
    add_library(afar_${stem} INTERFACE)
    target_include_directories(afar_${stem} INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}/${stem}
        ${PROJECT_SOURCE_DIR}/include
    )
    if(MINGW)
        target_compile_options(afar_${stem} INTERFACE -Wall -Wextra -Wpedantic -Werror)
    endif()
endfunction()

afar_add_interface_module(ui)

include(${PROJECT_SOURCE_DIR}/cmake/AfarWarnings.cmake)
include(${PROJECT_SOURCE_DIR}/cmake/Json.cmake)

add_library(afar_interfaces INTERFACE)
target_include_directories(afar_interfaces INTERFACE ${PROJECT_SOURCE_DIR}/include)

# VNA-002…005: SCPI Socket/COM + C1220/C2220 (закрытый набор из vna-c2220-scpi.md)
add_library(afar_vna STATIC
    vna/ScpiSocketTransport.cpp
    vna/ScpiComTransport.cpp
    vna/C2220Vna.cpp
)
target_include_directories(afar_vna PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/vna
    ${PROJECT_SOURCE_DIR}/include
)
target_link_libraries(afar_vna PUBLIC afar_interfaces)
if(WIN32)
    target_link_libraries(afar_vna PUBLIC ws2_32)
endif()
afar_enable_warnings(afar_vna)

add_library(afar_core STATIC
    core/RunId.cpp
    core/RunId.h
)
target_include_directories(afar_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/core
    ${PROJECT_SOURCE_DIR}/include
)
target_link_libraries(afar_core PUBLIC afar_interfaces)
afar_enable_warnings(afar_core)

add_library(afar_data STATIC
    data/RunConfig.cpp
    data/RunConfig.h
    data/AttenuatorCodes.cpp
    data/AttenuatorCodes.h
    data/SeriesDirectory.cpp
    data/SeriesDirectory.h
    data/RawS21Store.cpp
    data/RawS21Store.h
    data/RunEventLog.cpp
    data/RunEventLog.h
)
target_include_directories(afar_data PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/data
    ${PROJECT_SOURCE_DIR}/include
)
target_link_libraries(afar_data PUBLIC afar_core nlohmann_json::nlohmann_json)
afar_enable_warnings(afar_data)

add_library(afar_cal STATIC
    cal/Normalize.cpp
    cal/PhaseMath.cpp
    cal/DirectLut.cpp
    cal/InverseLut.cpp
)
target_include_directories(afar_cal PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/cal
    ${PROJECT_SOURCE_DIR}/include
)
afar_enable_warnings(afar_cal)

add_library(afar_qc STATIC
    qc/QualityGates.cpp
)
target_include_directories(afar_qc PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/qc
    ${PROJECT_SOURCE_DIR}/include
)
afar_enable_warnings(afar_qc)

# RPT-001…003: Parquet(AFARPQ)/манифест/PDF без Apache Arrow и OpenSSL.
# До afar_measure: оркестратор линкует report для Finalizing (LUT/PDF/manifest).
add_library(afar_report STATIC
    report/ParquetExport.cpp
    report/ParquetExport.h
    report/RawS21TableExport.cpp
    report/RawS21TableExport.h
    report/Manifest.cpp
    report/Manifest.h
    report/RunReportPdf.cpp
    report/RunReportPdf.h
)
target_include_directories(afar_report PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/report
    ${PROJECT_SOURCE_DIR}/include
)
target_link_libraries(afar_report PUBLIC afar_cal afar_data afar_qc)
afar_enable_warnings(afar_report)

# Версии и хеш коммита вшиваются при конфигурации. Из GUI git не вызывается.
find_package(Git QUIET)
set(AFAR_GIT_COMMIT "")
if(GIT_FOUND)
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" rev-parse HEAD
        WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
        OUTPUT_VARIABLE AFAR_GIT_COMMIT
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
        RESULT_VARIABLE _afar_git_rc
    )
    if(NOT _afar_git_rc EQUAL 0)
        set(AFAR_GIT_COMMIT "")
    endif()
endif()
unset(_afar_git_rc)

set(AFAR_JSON_VERSION "3.11.3")
if(DEFINED nlohmann_json_VERSION AND NOT nlohmann_json_VERSION STREQUAL "")
    set(AFAR_JSON_VERSION "${nlohmann_json_VERSION}")
endif()

add_library(afar_measure STATIC
    measure/RunStateMachine.cpp
    measure/RunStateMachine.h
    measure/ScanOrder.cpp
    measure/ScanOrder.h
    measure/MeasurementOrchestrator.cpp
    measure/MeasurementOrchestrator.h
)
target_include_directories(afar_measure PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/measure
    ${PROJECT_SOURCE_DIR}/include
)
target_link_libraries(afar_measure PUBLIC
    afar_data
    afar_cal
    afar_qc
    afar_interfaces
    afar_report
)
afar_enable_warnings(afar_measure)

add_library(afar_sim STATIC
    sim/VnaSimulator.cpp
    sim/DutSimulator.cpp
)
target_include_directories(afar_sim PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/sim
    ${PROJECT_SOURCE_DIR}/include
)
target_link_libraries(afar_sim PUBLIC afar_interfaces)
afar_enable_warnings(afar_sim)

add_library(afar_dut STATIC
    dut/StubDutController.cpp
)
target_include_directories(afar_dut PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/dut
    ${PROJECT_SOURCE_DIR}/include
)
target_link_libraries(afar_dut PUBLIC afar_interfaces)
afar_enable_warnings(afar_dut)

find_package(Qt6 REQUIRED COMPONENTS Widgets)
set(CMAKE_AUTOMOC ON)
if(NOT DEFINED Qt6_VERSION)
    set(Qt6_VERSION "unknown")
endif()
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/report/AfarBuildInfo.h.in
    ${CMAKE_CURRENT_BINARY_DIR}/report/AfarBuildInfo.h
    @ONLY
)
target_include_directories(afar_report PUBLIC ${CMAKE_CURRENT_BINARY_DIR}/report)

add_executable(AfarRxCalibrationStudio WIN32
    app/main.cpp
    ui/Theme.cpp
    ui/Theme.h
    ui/MainWindow.cpp
    ui/MainWindow.h
    ui/ConnectionBar.cpp
    ui/ConnectionBar.h
    ui/MeasureTab.cpp
    ui/MeasureTab.h
    ui/S21PlotWidget.cpp
    ui/S21PlotWidget.h
    ui/CodeMatrixTab.cpp
    ui/CodeMatrixTab.h
    ui/DataFormatsTab.cpp
    ui/DataFormatsTab.h
    ui/AcceptanceAtTab.cpp
    ui/AcceptanceAtTab.h
    ui/StartWizard.cpp
    ui/StartWizard.h
    ui/MeasureWorker.cpp
    ui/MeasureWorker.h
)
target_include_directories(AfarRxCalibrationStudio PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${PROJECT_SOURCE_DIR}/include
)
file(TO_CMAKE_PATH "${PROJECT_SOURCE_DIR}/examples" _afar_examples_dir)
target_compile_definitions(AfarRxCalibrationStudio PRIVATE
    "AFAR_EXAMPLES_DIR=\"${_afar_examples_dir}\""
)
unset(_afar_examples_dir)
# MinGW + AUTOMOC: вложенные кавычки в -D ломают генерацию moc_predefs.h.
set_target_properties(AfarRxCalibrationStudio PROPERTIES AUTOMOC_COMPILER_PREDEFINES OFF)
target_link_libraries(AfarRxCalibrationStudio PRIVATE
    Qt6::Widgets
    afar_measure
    afar_vna
    afar_sim
    afar_dut
    afar_report
    afar_data
    afar_cal
    afar_qc
)
if(WIN32)
    target_link_libraries(AfarRxCalibrationStudio PRIVATE Psapi)
endif()
afar_enable_warnings(AfarRxCalibrationStudio)
