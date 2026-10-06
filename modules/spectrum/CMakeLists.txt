find_path(SPECTRUM_FFTW3_INCLUDE_DIR fftw3.h REQUIRED)
find_library(SPECTRUM_FFTW3_LIBRARY NAMES fftw3f REQUIRED)
find_path(SPECTRUM_IIO_INCLUDE_DIR iio.h REQUIRED)
find_library(SPECTRUM_IIO_LIBRARY NAMES iio REQUIRED)
find_path(SPECTRUM_AD9361_INCLUDE_DIR ad9361.h REQUIRED)
find_library(SPECTRUM_AD9361_LIBRARY NAMES ad9361 REQUIRED)

add_library(spectrum_core STATIC
    src/spectrum_engine.cpp
    src/paths.cpp
    src/circular_buffer.cpp
    src/capture.cpp
    src/clip_monitor.cpp
    src/rx_calibration.cpp
    src/processing.cpp
    src/peleng.cpp
    src/finding.cpp
    src/network_stats.cpp
    src/board_finder.cpp
    src/demod.cpp
    src/dsp_utils.cpp
    src/audio_output.cpp
    src/playback.cpp
    src/tv_recorder.cpp
)
target_compile_features(spectrum_core PUBLIC cxx_std_20)
target_include_directories(spectrum_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${SPECTRUM_FFTW3_INCLUDE_DIR}
    ${SPECTRUM_IIO_INCLUDE_DIR}
    ${SPECTRUM_AD9361_INCLUDE_DIR}
)
target_include_directories(spectrum_core PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/miniaudio
)
target_link_libraries(spectrum_core PUBLIC
    ${SPECTRUM_FFTW3_LIBRARY}
    ${SPECTRUM_IIO_LIBRARY}
    ${SPECTRUM_AD9361_LIBRARY}
)

if(WIN32)
    target_compile_definitions(spectrum_core PRIVATE LIBIIO_STATIC)
    find_library(SPECTRUM_XML2_LIBRARY NAMES xml2 REQUIRED)
    target_link_libraries(spectrum_core PUBLIC
        ${SPECTRUM_XML2_LIBRARY}
        ws2_32
        iphlpapi
        ole32
        shell32
    )
endif()

# До этапа переноса в QOpenGLWidget этот target сохраняет исходный renderer.
# Он собирается как библиотека, но не запускает собственное окно.
find_package(OpenGL REQUIRED)
set(SPECTRUM_IMGUI_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui)
set(SPECTRUM_IMPLOT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/implot)

add_library(spectrum_imgui STATIC
    ${SPECTRUM_IMGUI_DIR}/imgui.cpp
    ${SPECTRUM_IMGUI_DIR}/imgui_draw.cpp
    ${SPECTRUM_IMGUI_DIR}/imgui_tables.cpp
    ${SPECTRUM_IMGUI_DIR}/imgui_widgets.cpp
    ${SPECTRUM_IMGUI_DIR}/backends/imgui_impl_opengl3.cpp
    ${SPECTRUM_IMPLOT_DIR}/implot.cpp
    ${SPECTRUM_IMPLOT_DIR}/implot_items.cpp
)
target_include_directories(spectrum_imgui PUBLIC
    ${SPECTRUM_IMGUI_DIR}
    ${SPECTRUM_IMGUI_DIR}/backends
    ${SPECTRUM_IMPLOT_DIR}
)
target_link_libraries(spectrum_imgui PUBLIC OpenGL::GL)
target_compile_options(spectrum_imgui PRIVATE -w)

add_library(spectrum_render STATIC src/display.cpp)
target_compile_features(spectrum_render PUBLIC cxx_std_20)
target_include_directories(spectrum_render PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(spectrum_render PUBLIC spectrum_core spectrum_imgui)

find_package(Qt6 REQUIRED COMPONENTS OpenGLWidgets)
add_library(spectrum_qt STATIC
    src/spectrum_opengl_widget.cpp
)
target_compile_features(spectrum_qt PUBLIC cxx_std_20)
target_include_directories(spectrum_qt PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(spectrum_qt PUBLIC spectrum_render Qt6::OpenGLWidgets)

file(GLOB SPECTRUM_CALIBRATION_TABLES
    ${CMAKE_CURRENT_SOURCE_DIR}/data/rx_calibration*.csv
)
set(SPECTRUM_CALIBRATION_TABLES ${SPECTRUM_CALIBRATION_TABLES} PARENT_SCOPE)
