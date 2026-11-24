#pragma once

#include <QByteArray>
#include <QHash>
#include <QMetaType>
#include <QString>
#include <functional>
#include <memory>
#include <optional>

#include "eyenix/tracker.hpp"

// What the app needs from an eye tracker. The real one talks USB through
// lib/; tests substitute a fake so the calibration flow runs without hardware.
// Every call may throw: eyenix::UsbError when the connection is gone, any
// other std::exception when the tracker refuses the request.
class Device {
public:
    virtual ~Device() = default;

    virtual QString name() = 0;
    virtual void setDisplayArea(const eyenix::DisplayArea &area) = 0;
    virtual void startGaze() = 0;
    virtual std::optional<eyenix::GazeSample> readGaze(int timeoutMs) = 0;

    virtual void calibrationStart() = 0;
    virtual void calibrationClear() = 0;
    virtual void calibrationAddPoint(eyenix::Point2 target) = 0;
    virtual void calibrationCompute() = 0;
    virtual void calibrationStop() = 0;
    virtual QByteArray calibration() = 0;
    virtual void setCalibration(const QByteArray &blob) = 0;

    // What game integration needs on top. A device without them (the tests'
    // fake) simply has no serial number, descriptions or camera.
    virtual QString serial() { return {}; }
    // The tracker's own answers to the questions a game asks about it, keyed
    // by request, to be passed on to games unchanged.
    virtual QHash<quint32, QByteArray> descriptions() { return {}; }
    virtual void startImages() {}
    virtual void stopImages() {}
    // The newest camera frame since the last call, if any.
    virtual std::optional<eyenix::ImageFrame> takeImage() { return std::nullopt; }
};

using DeviceDescriptions = QHash<quint32, QByteArray>;
Q_DECLARE_METATYPE(DeviceDescriptions)

// Opens a device or throws.
using DeviceFactory = std::function<std::unique_ptr<Device>()>;

DeviceFactory usbDeviceFactory();
