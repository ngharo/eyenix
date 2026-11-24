// Userspace driver for the Tobii Eye Tracker 5 (USB 2104:0313).
#pragma once

#include <cstdint>
#include <deque>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include "eyenix/display.hpp"
#include "eyenix/ttp.hpp"

struct libusb_context;
struct libusb_device_handle;

namespace eyenix {

/// USB-level failure (device missing, permission denied, I/O error).
struct UsbError : std::runtime_error {
    UsbError(const std::string& what, int code) : std::runtime_error(what), code(code) {}
    int code; // libusb error code
};

/// The tracker was unplugged. Destroy the Tracker and open a new one.
struct Disconnected : UsbError {
    using UsbError::UsbError;
};

/// The tracker answered a request with a non-zero status.
struct DeviceError : std::runtime_error {
    DeviceError(std::uint32_t op, std::uint32_t status);
    std::uint32_t op, status;
};

struct EyeSample {
    bool originValid = false;
    Point3 originMm;       // eye position, tracker coordinates
    bool gazeValid = false;
    Point2 gaze;           // on the display, 0..1, origin top-left
    double pupilMm = 0;    // pupil diameter; 0 if unknown
};

struct GazeSample {
    std::uint64_t timestampUs = 0; // device clock, see Tracker::clockUs()
    bool valid = false;            // combined gaze point is usable
    Point2 gaze;                   // on the display, 0..1, origin top-left
    EyeSample left, right;
};

/// Decodes a gaze stream payload. Unknown columns are ignored.
GazeSample decodeGazeSample(std::span<const std::uint8_t> payload);

/// One greyscale frame from the tracker's infrared camera (280 x 280, 8 bits per pixel).
/// These are pictures of the user's face: do not log or store them.
struct ImageFrame {
    std::uint64_t timestampUs = 0;
    std::uint32_t width = 0, height = 0, stride = 0; // stride: bytes per row
    std::vector<std::uint8_t> pixels;                // stride * height bytes, top row first
};

/// Decodes an image stream payload. Returns nullopt unless it is a complete 8-bit frame.
std::optional<ImageFrame> decodeImageFrame(std::span<const std::uint8_t> payload);

/// One connection to the tracker. Not thread-safe: use from a single thread.
class Tracker {
public:
    /// Opens the first Eye Tracker 5 found. Throws UsbError.
    Tracker();
    ~Tracker();
    Tracker(const Tracker&) = delete;
    Tracker& operator=(const Tracker&) = delete;

    /// Serial number, model and other identification strings, in device order.
    std::vector<std::string> deviceStrings();

    /// Device clock in microseconds since the tracker powered on.
    std::uint64_t clockUs();

    DisplayArea displayArea();
    void setDisplayArea(const DisplayArea& area);

    void startGaze();
    void stopGaze();
    /// Next gaze sample, or nullopt if none arrives within the timeout.
    std::optional<GazeSample> readGaze(int timeoutMs);

    /// Camera frames arrive alongside gaze samples, about 33 per second.
    void startImages();
    void stopImages();
    /// The newest camera frame received by readGaze() or request() since the last call.
    std::optional<ImageFrame> takeImage();

    // On-device calibration. Typical sequence, with gaze streaming on:
    //   calibrationStart, calibrationClear,
    //   calibrationAddPoint for each target (blocks ~0.6 s while the user looks at it),
    //   calibrationCompute, calibrationStop.
    // calibration() then returns a blob to keep and pass to setCalibration() later.
    void calibrationStart();
    void calibrationClear();
    void calibrationAddPoint(Point2 target);     // display coordinates, 0..1
    void calibrationDiscardPoint(Point2 target);
    void calibrationCompute();
    void calibrationStop();
    ttp::Bytes calibration();
    void setCalibration(std::span<const std::uint8_t> blob);

    /// Sends a request and returns the response. Throws DeviceError on a non-zero status.
    ttp::Message request(std::uint32_t op, std::span<const std::uint8_t> payload = {}, int timeoutMs = 3000);

private:
    std::optional<ttp::Message> readMessage(int timeoutMs);
    void close() noexcept;

    libusb_context* ctx_ = nullptr;
    libusb_device_handle* handle_ = nullptr;
    bool claimed_ = false, session_ = false;
    std::uint32_t seq_ = 0;
    ttp::FrameParser parser_;
    void handleStream(const ttp::Message& m);

    std::deque<GazeSample> gazeQueue_; // samples that arrived while waiting for a response
    std::optional<ImageFrame> image_;  // newest camera frame not yet taken
};

} // namespace eyenix
