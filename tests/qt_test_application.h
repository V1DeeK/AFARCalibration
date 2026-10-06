#pragma once

#include <QGuiApplication>

namespace afar::test {

inline QGuiApplication& guiApplication()
{
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) {
        qputenv("QT_QPA_PLATFORM", "offscreen");
    }
    static int argc = 1;
    static char name[] = "afar_test";
    static char* argv[] = {name, nullptr};
    static QGuiApplication application(argc, argv);
    return application;
}

}  // namespace afar::test
