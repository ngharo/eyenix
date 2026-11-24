#include "eyenix/tracker.hpp"

#include <chrono>
#include <cstdio>
#include <utility>

#include <libusb.h>

namespace eyenix {
namespace {

constexpr std::uint16_t kVid = 0x2104, kPid = 0x0313;
constexpr int kInterface = 0;
constexpr unsigned char kEpOut = 0x05, kEpIn = 0x83;
constexpr std::size_t kMaxQueuedSamples = 256;

// Protocol versions offered in the hello request, as sent by Tobii's software.
constexpr std::uint8_t kHelloPayload[] = {
    0x00, 0x00, 0x17, 0x00, 0x00, 0x00, 0x28, 0x00, 0x00, 0x00, 0x09, 0x00, 0x01, 0x00, 0x00, 0x00,
    0x01, 0x00, 0x01, 0x00, 0x01, 0x00, 0x02, 0x00, 0x01, 0x00, 0x03, 0x00, 0x01, 0x00, 0x04, 0x00,
    0x01, 0x00, 0x05, 0x00, 0x01, 0x00, 0x06, 0x00, 0x01, 0x00, 0x07, 0x00, 0x01, 0x00, 0x08,
};

[[noreturn]] void throwUsb(const char* what, int code) {
    std::string msg = std::string(what) + ": " + libusb_error_name(code);
    if (code == LIBUSB_ERROR_NO_DEVICE) throw Disconnected(msg, code);
    throw UsbError(msg, code);
}

std::string hex(std::uint32_t v) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "0x%x", v);
    return buf;
}

ttp::Bytes streamPayload(std::uint32_t stream, bool subscribe) {
    ttp::Writer w;
    w.u32(stream);
    if (subscribe) {
        const std::uint8_t zero[4] = {};
        w.item(0x17, zero);
    }
    return w.bytes();
}

ttp::Bytes pointPayload(Point2 p) {
    return ttp::Writer().fixed(p.x).fixed(p.y).u32(3).bytes(); // 3: both eyes (as sent by Tobii's software)
}

} // namespace

DeviceError::DeviceError(std::uint32_t op, std::uint32_t status)
    : std::runtime_error("tracker refused request " + hex(op) + " with status " + hex(status)), op(op), status(status) {}

GazeSample decodeGazeSample(std::span<const std::uint8_t> payload) {
    using namespace ttp;
    GazeSample s;
    bool combinedFlag = false;
    auto items = parseItems(payload);

    // Layout: sample header, then per column: (column header, column id, value).
    // A value is one scalar item, or a struct header followed by its members.
    for (std::size_t i = 1; i + 2 < items.size();) {
        const Item& head = items[i];
        if (head.type != Struct || (head.asUint() & 0xffff) != kColumn) { ++i; continue; }
        auto column = items[i + 1].asUint();
        const Item& value = items[i + 2];
        std::size_t members = value.type == Struct ? value.asUint() >> 16 : 0;
        if (i + 2 + members >= items.size() && members) break;
        i += 3 + members;

        auto num = [&](std::size_t k) { return (&value)[1 + k].asFixed(); };
        auto p2 = [&] { return members == 2 ? Point2{num(0), num(1)} : Point2{}; };
        auto p3 = [&] { return members == 3 ? Point3{num(0), num(1), num(2)} : Point3{}; };
        bool flag = value.asUint() == 1;

        switch (column) {
        case 1: s.timestampUs = value.asUint(); break;
        case 27: combinedFlag = flag; break;
        case 28: s.gaze = p2(); break;
        case 22: s.left.originValid = flag; break;
        case 2: s.left.originMm = p3(); break;
        case 29: s.left.gazeValid = flag; break;
        case 5: s.left.gaze = p2(); break;
        case 6: s.left.pupilMm = value.asFixed(); break;
        case 21: s.right.originValid = flag; break;
        case 8: s.right.originMm = p3(); break;
        case 30: s.right.gazeValid = flag; break;
        case 11: s.right.gaze = p2(); break;
        case 12: s.right.pupilMm = value.asFixed(); break;
        default: break;
        }
    }
    s.valid = combinedFlag;
    return s;
}

std::optional<ImageFrame> decodeImageFrame(std::span<const std::uint8_t> payload) {
    using namespace ttp;
    ImageFrame f;
    std::uint64_t bits = 0;
    std::span<const std::uint8_t> data;
    auto items = parseItems(payload);

    // Same layout as a gaze sample; every value is a single item.
    for (std::size_t i = 1; i + 2 < items.size(); i += 3) {
        if (items[i].type != Struct || (items[i].asUint() & 0xffff) != kColumn) return std::nullopt;
        const Item& value = items[i + 2];
        switch (items[i + 1].asUint()) {
        case 1: f.timestampUs = value.asUint(); break;
        case 2: bits = value.asUint(); break;
        case 3: f.width = static_cast<std::uint32_t>(value.asUint()); break;
        case 4: f.stride = static_cast<std::uint32_t>(value.asUint()); break;
        case 5: f.height = static_cast<std::uint32_t>(value.asUint()); break;
        case 6:
            if (value.type == Blob) data = value.asData();
            break;
        default: break;
        }
    }

    constexpr std::uint32_t kMaxSide = 4096;
    if (bits != 8 || f.width == 0 || f.height == 0 || f.width > kMaxSide || f.height > kMaxSide) return std::nullopt;
    if (f.stride < f.width || f.stride > 2 * kMaxSide) return std::nullopt;
    const std::size_t need = static_cast<std::size_t>(f.stride) * f.height;
    if (data.size() < need) return std::nullopt;
    f.pixels.assign(data.begin(), data.begin() + static_cast<std::ptrdiff_t>(need));
    return f;
}

void Tracker::handleStream(const ttp::Message& m) {
    if (m.magic != ttp::Stream) return;
    if (m.op == ttp::GazeStream) {
        if (gazeQueue_.size() >= kMaxQueuedSamples) gazeQueue_.pop_front();
        gazeQueue_.push_back(decodeGazeSample(m.payload));
    } else if (m.op == ttp::ImageStream) {
        if (auto f = decodeImageFrame(m.payload)) image_ = std::move(f);
    }
}

Tracker::Tracker() {
    int rc = libusb_init(&ctx_);
    if (rc != 0) throwUsb("libusb_init", rc);
    try {
        handle_ = libusb_open_device_with_vid_pid(ctx_, kVid, kPid);
        if (!handle_) throw UsbError("Tobii Eye Tracker 5 not found, or no permission to open it", LIBUSB_ERROR_NOT_FOUND);
        rc = libusb_claim_interface(handle_, kInterface);
        if (rc != 0) throwUsb("claim interface (is another program using the tracker?)", rc);
        claimed_ = true;
        rc = libusb_control_transfer(handle_, 0x41, 0x41, 0, 0, nullptr, 0, 1000); // open session
        if (rc < 0) throwUsb("open session", rc);
        session_ = true;

        // Discard anything left over from a previous user of the device.
        unsigned char junk[16384];
        int got = 0;
        while (libusb_bulk_transfer(handle_, kEpIn, junk, sizeof junk, &got, 20) == 0 && got > 0) {}

        request(ttp::Hello, kHelloPayload);
    } catch (...) {
        close();
        throw;
    }
}

Tracker::~Tracker() { close(); }

void Tracker::close() noexcept {
    if (handle_) {
        if (session_) libusb_control_transfer(handle_, 0x41, 0x42, 0, 0, nullptr, 0, 500); // close session
        if (claimed_) libusb_release_interface(handle_, kInterface);
        libusb_close(handle_);
    }
    if (ctx_) libusb_exit(ctx_);
    handle_ = nullptr;
    ctx_ = nullptr;
    claimed_ = session_ = false;
}

std::optional<ttp::Message> Tracker::readMessage(int timeoutMs) {
    using clock = std::chrono::steady_clock;
    auto deadline = clock::now() + std::chrono::milliseconds(timeoutMs);
    unsigned char buf[16384];
    for (;;) {
        if (auto m = parser_.next()) return m;
        auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - clock::now()).count();
        if (left <= 0) return std::nullopt;
        int got = 0;
        int rc = libusb_bulk_transfer(handle_, kEpIn, buf, sizeof buf, &got, static_cast<unsigned>(left));
        if (got > 0) parser_.feed({buf, static_cast<std::size_t>(got)});
        if (rc != 0 && rc != LIBUSB_ERROR_TIMEOUT) throwUsb("read", rc);
    }
}

ttp::Message Tracker::request(std::uint32_t op, std::span<const std::uint8_t> payload, int timeoutMs) {
    for (auto& packet : ttp::encodeRequest(++seq_, op, payload)) {
        int sent = 0;
        int rc = libusb_bulk_transfer(handle_, kEpOut, packet.data(), static_cast<int>(packet.size()), &sent, 5000);
        if (rc != 0) throwUsb("write", rc);
    }

    using clock = std::chrono::steady_clock;
    auto deadline = clock::now() + std::chrono::milliseconds(timeoutMs);
    for (;;) {
        auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - clock::now()).count();
        auto m = left > 0 ? readMessage(static_cast<int>(left)) : std::nullopt;
        if (!m) throw UsbError("no response to request " + hex(op), LIBUSB_ERROR_TIMEOUT);
        if (m->magic == ttp::Response && m->seq == seq_) {
            if (m->status != 0) throw DeviceError(op, m->status);
            return std::move(*m);
        }
        handleStream(*m); // other notifications are not used yet
    }
}

std::vector<std::string> Tracker::deviceStrings() {
    auto m = request(ttp::DeviceInfo);
    std::vector<std::string> out;
    for (const auto& it : ttp::parseItems(m.payload))
        if (it.type == ttp::String) {
            auto d = it.asData();
            out.emplace_back(d.begin(), d.end());
        }
    return out;
}

std::uint64_t Tracker::clockUs() {
    unsigned char b[8] = {};
    int rc = libusb_control_transfer(handle_, 0xc1, 0x46, 1, 0, b, sizeof b, 1000);
    if (rc != static_cast<int>(sizeof b)) throwUsb("read device clock", rc < 0 ? rc : LIBUSB_ERROR_IO);
    std::uint64_t v = 0;
    for (int i = 7; i >= 0; --i) v = (v << 8) | b[i];
    return v;
}

DisplayArea Tracker::displayArea() {
    auto m = request(ttp::GetDisplayArea);
    std::vector<double> v;
    for (const auto& it : ttp::parseItems(m.payload))
        if (it.type == ttp::Fixed42) v.push_back(it.asFixed());
    if (v.size() < 9) throw std::runtime_error("unexpected display area reply");
    return {{v[0], v[1], v[2]}, {v[3], v[4], v[5]}, {v[6], v[7], v[8]}};
}

void Tracker::setDisplayArea(const DisplayArea& a) {
    ttp::Writer w;
    w.point3(a.topLeft.x, a.topLeft.y, a.topLeft.z)
        .point3(a.topRight.x, a.topRight.y, a.topRight.z)
        .point3(a.bottomLeft.x, a.bottomLeft.y, a.bottomLeft.z)
        .structHeader(1, 0x0100) // trailer as sent by Tobii's software; meaning unknown
        .u32(0x3039);
    request(ttp::SetDisplayArea, w.bytes());
}

void Tracker::startGaze() { request(ttp::Subscribe, streamPayload(ttp::GazeStream, true)); }

void Tracker::stopGaze() {
    request(ttp::Unsubscribe, streamPayload(ttp::GazeStream, false));
    gazeQueue_.clear();
}

std::optional<GazeSample> Tracker::readGaze(int timeoutMs) {
    using clock = std::chrono::steady_clock;
    auto deadline = clock::now() + std::chrono::milliseconds(timeoutMs);
    for (;;) {
        if (!gazeQueue_.empty()) {
            auto s = gazeQueue_.front();
            gazeQueue_.pop_front();
            return s;
        }
        auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - clock::now()).count();
        auto m = readMessage(left > 0 ? static_cast<int>(left) : 0);
        if (!m) return std::nullopt;
        handleStream(*m);
    }
}

void Tracker::startImages() { request(ttp::Subscribe, streamPayload(ttp::ImageStream, true)); }

void Tracker::stopImages() {
    request(ttp::Unsubscribe, streamPayload(ttp::ImageStream, false));
    image_.reset();
}

std::optional<ImageFrame> Tracker::takeImage() { return std::exchange(image_, std::nullopt); }

void Tracker::calibrationStart() { request(ttp::CalStart); }
void Tracker::calibrationClear() { request(ttp::CalClear); }
void Tracker::calibrationAddPoint(Point2 p) { request(ttp::CalAddPoint, pointPayload(p), 10000); }
void Tracker::calibrationDiscardPoint(Point2 p) { request(ttp::CalDiscardPoint, pointPayload(p)); }
void Tracker::calibrationCompute() { request(ttp::CalCompute, {}, 10000); }
void Tracker::calibrationStop() { request(ttp::CalStop); }

ttp::Bytes Tracker::calibration() {
    auto m = request(ttp::CalGet, {}, 10000);
    for (const auto& it : ttp::parseItems(m.payload))
        if (it.type == ttp::Blob) {
            auto d = it.asData();
            return {d.begin(), d.end()};
        }
    throw std::runtime_error("unexpected calibration reply");
}

void Tracker::setCalibration(std::span<const std::uint8_t> blob) {
    request(ttp::CalSet, ttp::Writer().blob(blob).bytes(), 10000);
}

} // namespace eyenix
