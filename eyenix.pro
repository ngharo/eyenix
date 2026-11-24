QT += core gui network qml quick quickcontrols2

# The driver in lib/ uses std::span, so this needs C++20 rather than the usual C++17.
CONFIG += c++20 release link_pkgconfig
PKGCONFIG += libusb-1.0 libonnxruntime eigen3

# Head tracking uses two OpenCV modules. Its pkg-config file would link all of them.
QMAKE_CXXFLAGS += $$system(pkg-config --cflags opencv5)
LIBS += -lopencv_core -lopencv_imgproc
TARGET = eyenix
TEMPLATE = app

INCLUDEPATH += lib/include

HEADERS += \
    src/backend.h \
    src/control.h \
    src/session.h \
    src/store.h \
    src/device.h \
    src/gamereplies.h \
    src/gamemode.h \
    src/gameserver.h \
    src/wineprefix.h \
    src/gazefilter.h \
    src/models.h \
    src/theme.h \
    src/translator.h \
    src/trackerworker.h

SOURCES += \
    src/main.cpp \
    src/backend.cpp \
    src/cli.cpp \
    src/control.cpp \
    src/session.cpp \
    src/store.cpp \
    src/device.cpp \
    src/gamemode.cpp \
    src/gameserver.cpp \
    src/wineprefix.cpp \
    src/gazefilter.cpp \
    src/models.cpp \
    src/theme.cpp \
    src/translator.cpp \
    src/trackerworker.cpp \
    lib/src/display.cpp \
    lib/src/headpose.cpp \
    lib/src/vision/anchors.cpp \
    lib/src/vision/face_detector.cpp \
    lib/src/vision/face_landmarks.cpp \
    lib/src/vision/pose.cpp \
    lib/src/vision/preprocess.cpp \
    lib/src/vision/face_tracker.cpp \
    lib/src/tracker.cpp \
    lib/src/ttp.cpp

RESOURCES += src/resources.qrc
