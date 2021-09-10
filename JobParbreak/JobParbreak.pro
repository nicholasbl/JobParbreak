QT -= gui
QT += websockets

!versionAtLeast(QT_VERSION, 5.4.0):error("Use at least Qt version 5.4.0")

# assuming we are using qt5 or 6
greaterThan(QT_MINOR_VERSION, 11){
    CONFIG += c++17
} else {
    CONFIG += c++1z
}
CONFIG += console
CONFIG -= app_bundle

SOURCES += \
        jobsys.cpp \
        main.cpp

HEADERS += \
    jobsys.h
