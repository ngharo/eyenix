#include "backend.h"

#include <cmath>

#include "models.h"

namespace {

struct Target {
    double x, y;
    bool computeAfter;
};

// The seven targets Tobii's own software shows, in its order, computing after
// the first, the fourth and the last. Following it exactly keeps the tracker
// on a path that is known to work.
const Target kTargets[] = {
    {0.5, 0.5, true},
    {0.1, 0.9, false}, {0.5, 0.1, false}, {0.9, 0.9, true},
    {0.1, 0.1, false}, {0.9, 0.1, false}, {0.5, 0.9, true},
};
const int kTargetCount = int(std::size(kTargets));

// Lines closer together than this cannot be the tracker's marks on any real
// screen, and would make the screen absurdly large.
const double kMinimumSpacingPx = 40;

}

Backend::Backend(DeviceFactory factory, const QString &directory, QObject *parent, int retryMs)
    : QObject(parent), m_store(directory), m_worker(std::move(factory), retryMs) {
    m_store.loadAlignment(m_alignment, m_screenName);
    m_profile = m_store.profile();
    if (aligned())
        m_worker.setHeadDisplayArea(eyenix::displayAreaFromAlignment(m_alignment));

    connect(&m_worker, &TrackerWorker::described, this,
            [this](const QString &serial, const DeviceDescriptions &descriptions) {
        m_serial = serial;
        m_descriptions = descriptions;
        emit described();
    });
    connect(&m_worker, &TrackerWorker::idle, this, [this] {
        m_connected = false;
        m_idle = true;
        m_problem.clear();
        if (m_gazeValid) {
            m_gazeValid = false;
            emit gazeChanged();
        }
        emit connectionChanged();
        failCalibration(tr("The tracker was released."));
    });
    connect(&m_worker, &TrackerWorker::head, this, &Backend::headPose);
    connect(&m_worker, &TrackerWorker::headTrackingFailed, this, [this](const QString &reason) {
        setNotice(tr("Head tracking is not available: %1").arg(reason));
    });
    connect(&m_worker, &TrackerWorker::connected, this, [this](const QString &name) {
        m_idle = false;
        m_connected = true;
        m_deviceName = name;
        m_problem.clear();
        emit connectionChanged();
        sendStoredSettings();
    });
    connect(&m_worker, &TrackerWorker::disconnected, this, [this](const QString &reason) {
        const bool changed = m_connected || m_problem != reason;
        m_connected = false;
        m_problem = reason;
        if (m_gazeValid) {
            m_gazeValid = false;
            emit gazeChanged();
        }
        if (changed)
            emit connectionChanged();
        failCalibration(tr("The tracker was disconnected."));
    });
    connect(&m_worker, &TrackerWorker::finished, this,
            [this](int id, const QString &error, const QByteArray &result) {
        if (const auto handler = m_handlers.take(id))
            handler(error, result);
    });
    connect(&m_worker, &TrackerWorker::gaze, this, [this](const eyenix::GazeSample &sample) {
        emit sampled(sample);
        m_gazeValid = sample.valid;
        if (sample.valid)
            m_gaze = m_filter.filter({sample.gaze.x, sample.gaze.y}, sample.timestampUs / 1e6);

        double depth = 0;
        int eyes = 0;
        for (const auto *eye : {&sample.left, &sample.right})
            if (eye->originValid) {
                depth += eye->originMm.z;
                ++eyes;
            }
        m_distanceCm = eyes ? depth / eyes / 10 : 0;
        emit gazeChanged();
    });
    m_worker.start();
}

void Backend::setTrackerWanted(bool wanted) {
    m_worker.setWanted(wanted);
    if (wanted && m_idle) {
        m_idle = false; // on its way back; "connected" follows when it is there
        emit connectionChanged();
    }
}

bool Backend::setHeadTracking(bool on) {
    const QString models = on ? modelDirectory() : QString();
    m_worker.setHeadTracking(on, models);
    return !on || !models.isEmpty();
}

int Backend::calibrationPointCount() const { return kTargetCount; }

QPointF Backend::calibrationPoint(int index) const {
    return index >= 0 && index < kTargetCount ? QPointF(kTargets[index].x, kTargets[index].y) : QPointF();
}

void Backend::run(TrackerWorker::Command command, Handler handler) {
    const int id = m_worker.post(std::move(command));
    if (handler)
        m_handlers.insert(id, std::move(handler));
}

void Backend::setNotice(const QString &notice) {
    if (notice == m_notice)
        return;
    m_notice = notice;
    emit noticeChanged();
}

double Backend::defaultMarkSpacing(double pixelsPerMm) const { return eyenix::kMarkerSpacingMm * pixelsPerMm; }

QVariantMap Backend::screenSize(double leftPx, double rightPx, double widthPx, double heightPx) const {
    if (rightPx - leftPx < kMinimumSpacingPx)
        return {};
    const double mm = eyenix::kMarkerSpacingMm / (rightPx - leftPx);
    return {{"widthMm", widthPx * mm},
            {"heightMm", heightPx * mm},
            {"diagonalInches", std::hypot(widthPx, heightPx) * mm / 25.4}};
}

void Backend::applyAlignment(double leftPx, double rightPx, double widthPx, double heightPx,
                             const QString &screenName) {
    if (rightPx - leftPx < kMinimumSpacingPx || widthPx <= 0 || heightPx <= 0)
        return;
    m_alignment.leftMarkPx = leftPx;
    m_alignment.rightMarkPx = rightPx;
    m_alignment.screenWidthPx = widthPx;
    m_alignment.screenHeightPx = heightPx;
    m_screenName = screenName;

    if (!m_store.saveAlignment(m_alignment, screenName))
        setNotice(tr("The alignment could not be saved in %1.").arg(m_store.directory()));
    m_worker.setHeadDisplayArea(eyenix::displayAreaFromAlignment(m_alignment));
    emit alignmentChanged();

    if (m_connected)
        sendStoredSettings();
}

bool Backend::useProfile(const QString &name) {
    if (m_calibrating || !m_store.hasCalibration(name))
        return false;
    if (name == m_profile)
        return true;
    m_profile = name;
    if (!m_store.setProfile(name))
        setNotice(tr("The profile choice could not be saved in %1.").arg(m_store.directory()));
    emit calibrationChanged();
    if (m_connected)
        sendCalibration();
    return true;
}

void Backend::sendStoredSettings() {
    setNotice({});
    if (aligned()) {
        const auto area = eyenix::displayAreaFromAlignment(m_alignment);
        run([area](Device &device) { device.setDisplayArea(area); return QByteArray(); },
            [this](const QString &error, const QByteArray &) {
                if (!error.isEmpty() && m_connected)
                    setNotice(tr("The tracker refused the screen alignment: %1").arg(error));
            });
    }
    sendCalibration();
}

void Backend::sendCalibration() {
    const QByteArray blob = m_store.calibration(m_profile);
    if (blob.isEmpty())
        return;
    run([blob](Device &device) { device.setCalibration(blob); return QByteArray(); },
        [this](const QString &error, const QByteArray &) {
            if (!error.isEmpty() && m_connected)
                setNotice(tr("The tracker refused the saved calibration. Calibrate again."));
        });
}

void Backend::beginCalibration(const QString &profile) {
    if (m_calibrating)
        return;
    if (!m_connected) {
        emit calibrationFailed(tr("The tracker is not connected."));
        return;
    }
    const QString target = profile.isEmpty() ? m_profile : profile;
    if (!Store::validProfileName(target)) {
        emit calibrationFailed(tr("A profile name can have letters, digits, spaces, dashes and underscores."));
        return;
    }
    m_target = target;
    m_calibrating = true;
    const int session = ++m_session;
    emit calibrationChanged();
    run([](Device &device) {
            device.calibrationStart();
            device.calibrationClear();
            return QByteArray();
        },
        [this, session](const QString &error, const QByteArray &) {
            if (session == m_session && !error.isEmpty())
                failCalibration(error);
        });
}

void Backend::collectPoint(int index) {
    if (!m_calibrating || index < 0 || index >= kTargetCount)
        return;
    const Target target = kTargets[index];
    const bool last = index == kTargetCount - 1;
    const int session = m_session;
    run([target, last](Device &device) {
            device.calibrationAddPoint({target.x, target.y});
            if (target.computeAfter)
                device.calibrationCompute();
            if (!last)
                return QByteArray();
            const QByteArray blob = device.calibration();
            device.calibrationStop();
            return blob;
        },
        [this, session, index, last](const QString &error, const QByteArray &blob) {
            if (session != m_session || !m_calibrating)
                return;
            if (!error.isEmpty()) {
                failCalibration(error);
                return;
            }
            emit pointCollected(index);
            if (!last)
                return;
            if (!m_store.saveCalibration(m_target, blob)) {
                // The tracker has the new calibration for now, but it would be gone after a replug.
                failCalibration(tr("The calibration could not be saved in %1.").arg(m_store.directory()));
                return;
            }
            m_calibrating = false;
            m_profile = m_target;
            if (!m_store.setProfile(m_profile))
                setNotice(tr("The profile choice could not be saved in %1.").arg(m_store.directory()));
            emit calibrationChanged();
            emit calibrationSaved();
        });
}

void Backend::cancelCalibration() {
    if (!m_calibrating)
        return;
    endCalibration();
}

void Backend::failCalibration(const QString &message) {
    if (!m_calibrating)
        return;
    endCalibration();
    emit calibrationFailed(message);
}

// Leaves calibration mode on the tracker and puts back the calibration it had,
// since starting a new one cleared it.
void Backend::endCalibration() {
    m_calibrating = false;
    ++m_session;
    emit calibrationChanged();
    if (!m_connected)
        return;
    run([](Device &device) { device.calibrationStop(); return QByteArray(); });
    sendCalibration();
}
