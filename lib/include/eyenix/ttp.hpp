// TTP wire format for the Tobii Eye Tracker 5. See docs/protocol-notes.md.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace eyenix::ttp {

using Bytes = std::vector<std::uint8_t>;

enum Magic : std::uint32_t { Request = 0x51, Response = 0x52, Stream = 0x53, Notify = 0x4e };

enum Op : std::uint32_t {
    Hello = 0x3e8,
    CalStart = 0x3f2,
    CalStop = 0x3fc,
    CalAddPoint = 0x406,
    CalClear = 0x424,
    CalCompute = 0x42e,
    CalDiscardPoint = 0x438,
    CalGet = 0x44c,
    CalSet = 0x456,
    Subscribe = 0x4c4,
    Unsubscribe = 0x4ce,
    DeviceInfo = 0x58c,
    GetDisplayArea = 0x596,
    SetDisplayArea = 0x5a0,
};

enum StreamId : std::uint32_t { GazeStream = 0x500, PresenceStream = 0x504, ImageStream = 0x50e };

enum ItemType : std::uint8_t {
    U32 = 2,
    Fixed16 = 3, // u32, value / 2^16
    Fixed42 = 4, // s64, value / 2^42
    Struct = 5,  // u16 member count, u16 struct id
    U64 = 6,
    String = 0x14,
    Blob = 0x15,
};

constexpr std::uint16_t kPoint2 = 8000, kPoint3 = 8001, kSample = 3000, kColumn = 3001;

struct Message {
    std::uint32_t magic = 0, seq = 0, op = 0, status = 0;
    Bytes payload;
};

/// Builds a request payload.
class Writer {
public:
    Writer() : buf_{0, 0} {}
    Writer& u32(std::uint32_t v);
    Writer& fixed(double v);
    Writer& structHeader(std::uint16_t count, std::uint16_t id);
    Writer& point3(double x, double y, double z);
    Writer& blob(std::span<const std::uint8_t> data);
    Writer& item(std::uint8_t type, std::span<const std::uint8_t> value);
    const Bytes& bytes() const { return buf_; }

private:
    Bytes buf_;
};

struct Item {
    std::uint8_t type;
    std::span<const std::uint8_t> value;

    std::uint64_t asUint() const;  // big-endian integer of any width up to 8
    double asFixed() const;        // Fixed42 or Fixed16
    std::span<const std::uint8_t> asData() const; // String / Blob body
};

/// Splits a payload into items. Stops at the first malformed item.
std::vector<Item> parseItems(std::span<const std::uint8_t> payload);

/// Bulk-OUT packets for a request, to be written in order, one transfer each.
/// Large requests are split into packets of at most 4095 bytes.
std::vector<Bytes> encodeRequest(std::uint32_t seq, std::uint32_t op, std::span<const std::uint8_t> payload);

/// Reassembles messages from bulk-IN reads.
class FrameParser {
public:
    void feed(std::span<const std::uint8_t> data);
    /// Next complete message. Throws std::runtime_error if the stream is corrupt.
    std::optional<Message> next();

private:
    Bytes buf_;
    std::optional<Message> partial_; // header seen, payload still arriving
    std::size_t expected_ = 0;       // payload length announced in the header
};

} // namespace eyenix::ttp
