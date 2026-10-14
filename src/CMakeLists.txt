include(${PROJECT_SOURCE_DIR}/cmake/AfarWarnings.cmake)

function(afar_add_interface_module stem)
    add_library(afar_${stem} INTERFACE)
    target_include_directories(afar_${stem} INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}/${stem}
        ${PROJECT_SOURCE_DIR}/include
    )
    if(AFAR_WARNING_FLAGS)
        target_compile_options(afar_${stem} INTERFACE ${AFAR_WARNING_FLAGS})
    endif()
endfunction()

afar_add_interface_module(ui)
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
afar_enable_warnings(afar_vna)

add_library(afar_core STATIC
    core/RunId.cpp
    core/RunId.h
    core/NumberText.cpp
    core/NumberText.h
    core/ProcessMetrics.cpp
    core/ProcessMetrics.h
)
target_include_directories(afar_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/core
    ${PROJECT_SOURCE_DIR}/include
)
target_link_libraries(afar_core PUBLIC afar_interfaces)
if(WIN32)
    target_link_libraries(afar_core PRIVATE Psapi)
endif()
afar_enable_warnings(afar_core)

target_link_libraries(afar_vna PUBLIC afar_interfaces afar_core)
if(WIN32)
    target_link_libraries(afar_vna PUBLIC ws2_32)
endif()

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

find_package(Qt6 REQUIRED COMPONENTS Widgets OpenGLWidgets)

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
    report/TwoPortExport.cpp
    report/TwoPortExport.h
)
target_include_directories(afar_report PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/report
    ${PROJECT_SOURCE_DIR}/include
)
target_link_libraries(afar_report PUBLIC afar_cal afar_data afar_qc Qt6::Gui)
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

# Обмен с PS изделия (docs/contracts/ps-json-protocol.md): кодек без Qt и
# асинхронная связь TCP + UDP на Qt Network.
add_library(afar_ps_protocol STATIC
    ps/PsProtocol.cpp
    ps/PsProtocol.h
    ps/PsServiceCheck.cpp
    ps/PsServiceCheck.h
)
target_include_directories(afar_ps_protocol PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/ps)
target_link_libraries(afar_ps_protocol PUBLIC nlohmann_json::nlohmann_json)
afar_enable_warnings(afar_ps_protocol)

find_package(Qt6 REQUIRED COMPONENTS Network)

add_library(afar_ps STATIC
    ps/PsLink.cpp
    ps/PsLink.h
    ps/MacResolver.cpp
    ps/MacResolver.h
    ps/PsSession.cpp
    ps/PsSession.h
    ps/PsServiceCheckRunner.cpp
    ps/PsServiceCheckRunner.h
)
set_target_properties(afar_ps PROPERTIES AUTOMOC ON)
target_include_directories(afar_ps PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/ps)
target_link_libraries(afar_ps PUBLIC afar_ps_protocol Qt6::Network)
afar_enable_warnings(afar_ps)

# IDutController через PS изделия: блокирующий драйвер для рабочего потока.
add_library(afar_dut_ps STATIC
    dut/PsDutController.cpp
    dut/PsDutController.h
)
target_include_directories(afar_dut_ps PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/dut
    ${PROJECT_SOURCE_DIR}/include
)
target_link_libraries(afar_dut_ps PUBLIC afar_interfaces afar_ps_protocol Qt6::Network)
afar_enable_warnings(afar_dut_ps)

add_library(afar_dut STATIC
    dut/StubDutController.cpp
)
target_include_directories(afar_dut PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/dut
    ${PROJECT_SOURCE_DIR}/include
)
target_link_libraries(afar_dut PUBLIC afar_interfaces)
afar_enable_warnings(afar_dut)

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

# Интерфейс программы — статическая библиотека: из неё собираются и приложение, и автотесты окна.
add_library(afar_app_ui STATIC
    ui/Theme.cpp
    ui/Theme.h
    ui/UiStyle.cpp
    ui/UiStyle.h
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
    ui/BoardParametersPage.cpp
    ui/BoardParametersPage.h
    ui/ParametersTab.cpp
    ui/ParametersTab.h
    ui/ConnectionTab.cpp
    ui/ConnectionTab.h
    ui/FullScanPanel.cpp
    ui/FullScanPanel.h
    ui/PsExchangeView.cpp
    ui/PsExchangeView.h
    ui/PsJournalDialog.cpp
    ui/PsJournalDialog.h
    ui/StressTestPanel.cpp
    ui/StressTestPanel.h
    ui/TestRunTab.cpp
    ui/TestRunTab.h
    ui/PsModulesPage.cpp
    ui/PsModulesPage.h
    ui/PsPacketPage.cpp
    ui/PsPacketPage.h
    ui/PsServiceCheckPage.cpp
    ui/PsServiceCheckPage.h
)
target_include_directories(afar_app_ui PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/ui
    ${PROJECT_SOURCE_DIR}/include
)
file(TO_CMAKE_PATH "${PROJECT_SOURCE_DIR}/examples" _afar_examples_dir)
target_compile_definitions(afar_app_ui PRIVATE
    "AFAR_EXAMPLES_DIR=\"${_afar_examples_dir}\""
)
unset(_afar_examples_dir)
# MinGW + AUTOMOC: вложенные кавычки в -D ломают генерацию moc_predefs.h.
set_target_properties(afar_app_ui PROPERTIES AUTOMOC_COMPILER_PREDEFINES OFF)
target_link_libraries(afar_app_ui PUBLIC
    Qt6::Widgets
    Qt6::OpenGLWidgets
    OpenGL::GL
    spectrum_core
    spectrum_qt
    afar_measure
    afar_vna
    afar_sim
    afar_dut
    afar_report
    afar_data
    afar_cal
    afar_qc
    afar_ps
    afar_dut_ps
)
if(WIN32)
    target_link_libraries(afar_app_ui PUBLIC Psapi)
endif()
afar_enable_warnings(afar_app_ui)

add_executable(AfarRxCalibrationStudio WIN32 MACOSX_BUNDLE app/main.cpp)
target_include_directories(AfarRxCalibrationStudio PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${PROJECT_SOURCE_DIR}/include
)
target_link_libraries(AfarRxCalibrationStudio PRIVATE afar_app_ui)
foreach(_spectrum_table IN LISTS SPECTRUM_CALIBRATION_TABLES)
    get_filename_component(_spectrum_table_name "${_spectrum_table}" NAME)
    add_custom_command(TARGET AfarRxCalibrationStudio POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory
                $<TARGET_FILE_DIR:AfarRxCalibrationStudio>/data/spectrum
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${_spectrum_table}"
                $<TARGET_FILE_DIR:AfarRxCalibrationStudio>/data/spectrum/${_spectrum_table_name}
    )
endforeach()
unset(_spectrum_table)
unset(_spectrum_table_name)
afar_enable_warnings(AfarRxCalibrationStudio)

# macOS: .app с понятным именем; примеры кладутся в Contents/Resources.
if(APPLE)
    set_target_properties(AfarRxCalibrationStudio PROPERTIES
        MACOSX_BUNDLE_BUNDLE_NAME "AFAR RX Calibration Studio"
        MACOSX_BUNDLE_GUI_IDENTIFIER "ru.afar.rxcalibrationstudio"
        MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
    )
endif()

# Установка: cmake --install build --prefix <каталог>. Qt-библиотеки рядом с
# программой разворачивает скрипт Qt (Windows/macOS с Qt 6.3, Linux с Qt 6.5);
# на более старом Qt для Linux берутся системные пакеты Qt 6.
include(GNUInstallDirs)
install(TARGETS AfarRxCalibrationStudio
    BUNDLE DESTINATION .
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
if(APPLE)
    install(DIRECTORY ${PROJECT_SOURCE_DIR}/examples/
        DESTINATION AfarRxCalibrationStudio.app/Contents/Resources/examples)
elseif(WIN32)
    install(DIRECTORY ${PROJECT_SOURCE_DIR}/examples/
        DESTINATION ${CMAKE_INSTALL_BINDIR}/examples)
else()
    install(DIRECTORY ${PROJECT_SOURCE_DIR}/examples/
        DESTINATION ${CMAKE_INSTALL_DATADIR}/AfarRxCalibrationStudio/examples)
endif()
if(Qt6_VERSION VERSION_GREATER_EQUAL 6.5)
    qt_generate_deploy_app_script(
        TARGET AfarRxCalibrationStudio
        OUTPUT_SCRIPT _afar_deploy_script
        NO_UNSUPPORTED_PLATFORM_ERROR
    )
    install(SCRIPT ${_afar_deploy_script})
elseif(Qt6_VERSION VERSION_GREATER_EQUAL 6.3 AND (WIN32 OR APPLE))
    qt_generate_deploy_app_script(
        TARGET AfarRxCalibrationStudio
        FILENAME_VARIABLE _afar_deploy_script
        NO_UNSUPPORTED_PLATFORM_ERROR
    )
    install(SCRIPT ${_afar_deploy_script})
endif()
