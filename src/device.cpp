#include "device.h"

namespace {

class UsbDevice : public Device {
public:
    QString name() override {
        // The second identification string is the model; the first is the serial number.
        const auto strings = m_tracker.deviceStrings();
        return strings.size() > 1 ? QString::fromStdString(strings[1]).replace('_', ' ')
                                  : QStringLiteral("Tobii Eye Tracker 5");
    }
    void setDisplayArea(const eyenix::DisplayArea &area) override { m_tracker.setDisplayArea(area); }
    void startGaze() override { m_tracker.startGaze(); }
    std::optional<eyenix::GazeSample> readGaze(int timeoutMs) override { return m_tracker.readGaze(timeoutMs); }

    void calibrationStart() override { m_tracker.calibrationStart(); }
    void calibrationClear() override { m_tracker.calibrationClear(); }
    void calibrationAddPoint(eyenix::Point2 target) override { m_tracker.calibrationAddPoint(target); }
    void calibrationCompute() override { m_tracker.calibrationCompute(); }
    void calibrationStop() override { m_tracker.calibrationStop(); }
    QByteArray calibration() override {
        const auto blob = m_tracker.calibration();
        return QByteArray(reinterpret_cast<const char *>(blob.data()), qsizetype(blob.size()));
    }
    void setCalibration(const QByteArray &blob) override {
        m_tracker.setCalibration({reinterpret_cast<const std::uint8_t *>(blob.constData()), size_t(blob.size())});
    }

    QString serial() override {
        const auto strings = m_tracker.deviceStrings();
        return strings.empty() ? QString() : QString::fromStdString(strings[0]);
    }
    QHash<quint32, QByteArray> descriptions() override {
        // Device information, configuration, capabilities and the list of streams.
        QHash<quint32, QByteArray> out;
        for (const quint32 op : {0x058cu, 0x0532u, 0x0546u, 0x04b0u}) {
            try {
                const auto reply = m_tracker.request(op);
                out.insert(op, QByteArray(reinterpret_cast<const char *>(reply.payload.data()), qsizetype(reply.payload.size())));
            } catch (const eyenix::DeviceError &) {
                // Refused: games get the stock answer for this one.
            }
        }
        return out;
    }
    void startImages() override { m_tracker.startImages(); }
    void stopImages() override { m_tracker.stopImages(); }
    std::optional<eyenix::ImageFrame> takeImage() override { return m_tracker.takeImage(); }

private:
    eyenix::Tracker m_tracker;
};

}

DeviceFactory usbDeviceFactory() {
    return [] { return std::make_unique<UsbDevice>(); };
}
