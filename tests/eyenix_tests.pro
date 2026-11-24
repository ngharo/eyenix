QT += core gui network qml quick quickcontrols2 testlib
CONFIG += c++20 testcase link_pkgconfig
PKGCONFIG += libusb-1.0 libonnxruntime eigen3
QMAKE_CXXFLAGS += $$system(pkg-config --cflags opencv5)
LIBS += -lopencv_core -lopencv_imgproc
TARGET = eyenix_tests
TEMPLATE = app

INCLUDEPATH += ../src ../lib/include

HEADERS += ../src/backend.h ../src/control.h ../src/session.h ../src/store.h ../src/device.h ../src/gazefilter.h ../src/gameserver.h ../src/gamemode.h ../src/wineprefix.h ../src/models.h ../src/theme.h ../src/translator.h \
    ../src/trackerworker.h
SOURCES += eyenix_tests.cpp ../src/backend.cpp ../src/control.cpp ../src/session.cpp ../src/store.cpp ../src/device.cpp ../src/gazefilter.cpp ../src/gameserver.cpp ../src/gamemode.cpp ../src/wineprefix.cpp ../src/models.cpp ../src/theme.cpp \
    ../src/translator.cpp ../src/trackerworker.cpp \
    ../lib/src/headpose.cpp ../lib/src/vision/anchors.cpp ../lib/src/vision/face_detector.cpp ../lib/src/vision/face_landmarks.cpp ../lib/src/vision/pose.cpp ../lib/src/vision/preprocess.cpp ../lib/src/vision/face_tracker.cpp \
    ../lib/src/display.cpp ../lib/src/tracker.cpp ../lib/src/ttp.cpp
