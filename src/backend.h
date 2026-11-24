#pragma once

#include <QHash>
#include <QObject>
#include <QPointF>
#include <QVariantMap>
#include <functional>

#include "device.h"
#include "gazefilter.h"
#include "store.h"
#include "trackerworker.h"

// The app's state: the tracker connection, where the screen is relative to
// the tracker (alignment), the calibration profiles, and the live gaze point.
// Alignment and the current profile's calibration are kept on disk and sent
// to the tracker every time it connects, because the tracker does not
// remember them.
class Backend : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool connected READ connected NOTIFY connectionChanged)
    Q_PROPERTY(QString deviceName READ deviceName NOTIFY connectionChanged)
    Q_PROPERTY(QString problem READ problem NOTIFY connectionChanged)
    Q_PROPERTY(bool idle READ idle NOTIFY connectionChanged)
    Q_PROPERTY(QString notice READ notice NOTIFY noticeChanged)

    Q_PROPERTY(bool aligned READ aligned NOTIFY alignmentChanged)
    Q_PROPERTY(QString screenName READ screenName NOTIFY alignmentChanged)
    Q_PROPERTY(double markLeft READ markLeft NOTIFY alignmentChanged)
    Q_PROPERTY(double markRight READ markRight NOTIFY alignmentChanged)
    Q_PROPERTY(double alignedWidth READ alignedWidth NOTIFY alignmentChanged)
    Q_PROPERTY(double alignedHeight READ alignedHeight NOTIFY alignmentChanged)

    Q_PROPERTY(QString profile READ profile NOTIFY calibrationChanged)
    Q_PROPERTY(QStringList profiles READ profiles NOTIFY calibrationChanged)
    Q_PROPERTY(bool calibrated READ calibrated NOTIFY calibrationChanged)
    Q_PROPERTY(bool calibrating READ calibrating NOTIFY calibrationChanged)
    Q_PROPERTY(int calibrationPointCount READ calibrationPointCount CONSTANT)

    Q_PROPERTY(bool gazeValid READ gazeValid NOTIFY gazeChanged)
    Q_PROPERTY(double gazeX READ gazeX NOTIFY gazeChanged)
    Q_PROPERTY(double gazeY READ gazeY NOTIFY gazeChanged)
    Q_PROPERTY(double distanceCm READ distanceCm NOTIFY gazeChanged)

public:
    // The directory is the Store's; empty means the app's config directory.
    // Tests pass a temp directory and a fake device.
    explicit Backend(DeviceFactory factory, const QString &directory = {}, QObject *parent = nullptr,
                     int retryMs = 1000);

    bool connected() const { return m_connected; }
    QString deviceName() const { return m_deviceName; }
    QString problem() const { return m_problem; }
    // The tracker was let go because nothing needs it right now.
    bool idle() const { return m_idle; }

    // Whether anything needs the tracker. Without a need it is released, so
    // it can power down. Needed from the start.
    void setTrackerWanted(bool wanted);
    // Head pose from the tracker's camera, for games. Costs CPU, so only on request.
    // Returns false when the face models are not installed.
    bool setHeadTracking(bool on);

    // What the connected tracker said about itself; empty until it has connected once.
    QString serial() const { return m_serial; }
    DeviceDescriptions descriptions() const { return m_descriptions; }
    const eyenix::Alignment &alignment() const { return m_alignment; }
    Store &store() { return m_store; }
    QString notice() const { return m_notice; }

    bool aligned() const { return m_alignment.rightMarkPx > m_alignment.leftMarkPx; }
    QString screenName() const { return m_screenName; }
    double markLeft() const { return m_alignment.leftMarkPx; }
    double markRight() const { return m_alignment.rightMarkPx; }
    double alignedWidth() const { return m_alignment.screenWidthPx; }
    double alignedHeight() const { return m_alignment.screenHeightPx; }

    QString profile() const { return m_profile; }
    QStringList profiles() const { return m_store.profiles(); }
    bool calibrated() const { return m_store.hasCalibration(m_profile); }
    bool calibrating() const { return m_calibrating; }
    int calibrationPointCount() const;

    bool gazeValid() const { return m_gazeValid; }
    double gazeX() const { return m_gaze.x; }
    double gazeY() const { return m_gaze.y; }
    double distanceCm() const { return m_distanceCm; }

    // Distance in pixels between the two alignment lines that matches the
    // tracker's marks on a screen with this many pixels per millimetre.
    Q_INVOKABLE double defaultMarkSpacing(double pixelsPerMm) const;
    // The physical screen size the given line positions imply: widthMm,
    // heightMm and diagonalInches, for the user to check against reality.
    Q_INVOKABLE QVariantMap screenSize(double leftPx, double rightPx, double widthPx, double heightPx) const;
    Q_INVOKABLE void applyAlignment(double leftPx, double rightPx, double widthPx, double heightPx,
                                    const QString &screenName);

    // Switches to a calibrated profile and sends its calibration to the tracker.
    Q_INVOKABLE bool useProfile(const QString &name);
    // A name for a new profile: "Profile 2", "Profile 3", …
    Q_INVOKABLE QString unusedProfileName() const { return m_store.unusedProfileName(); }

    // Calibration targets as fractions of the screen, in the order to show them.
    Q_INVOKABLE QPointF calibrationPoint(int index) const;
    // Calibrates the named profile, creating it if it is new, and switches to
    // it once the calibration is saved. No name means the current profile.
    Q_INVOKABLE void beginCalibration(const QString &profile = {});
    // Call while the user is looking at target `index`; pointCollected follows.
    Q_INVOKABLE void collectPoint(int index);
    Q_INVOKABLE void cancelCalibration();

signals:
    void connectionChanged();
    void noticeChanged();
    void alignmentChanged();
    void calibrationChanged();
    void gazeChanged();
    // Every sample as the tracker sent it, unsmoothed. For games.
    void sampled(const eyenix::GazeSample &sample);
    // One per camera frame while head tracking is on. Not found: no face in view.
    void headPose(const eyenix::HeadPose &pose, bool found);
    void described();
    void pointCollected(int index);
    void calibrationFailed(const QString &message);
    void calibrationSaved();

private:
    using Handler = std::function<void(const QString &error, const QByteArray &result)>;

    void run(TrackerWorker::Command command, Handler handler = {});
    void sendStoredSettings();
    void sendCalibration();
    void failCalibration(const QString &message);
    void endCalibration();
    void setNotice(const QString &notice);

    Store m_store;
    TrackerWorker m_worker;
    QHash<int, Handler> m_handlers;

    bool m_connected = false, m_idle = false;
    QString m_deviceName, m_problem, m_notice, m_serial;
    DeviceDescriptions m_descriptions;

    eyenix::Alignment m_alignment;
    QString m_screenName;

    QString m_profile;
    QString m_target; // the profile being calibrated
    bool m_calibrating = false;
    int m_session = 0; // tells a late answer from a cancelled calibration apart

    GazeFilter m_filter;
    bool m_gazeValid = false;
    GazeFilter::Point m_gaze;
    double m_distanceCm = 0;
};
