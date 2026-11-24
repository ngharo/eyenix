#include "eyenix/ttp.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace eyenix::ttp {
namespace {

constexpr std::size_t kEnvelope = 8, kHeader = 24;
constexpr std::size_t kMaxMessage = 8 * 1024 * 1024;
constexpr std::size_t kMaxPacket = 4095; // largest bulk-OUT packet Tobii's software sends

void putBe(Bytes& b, std::uint64_t v, int n) {
    for (int i = n - 1; i >= 0; --i) b.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
}

void putLe32(Bytes& b, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) b.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
}

std::uint64_t getBe(std::span<const std::uint8_t> s, std::size_t off, int n) {
    std::uint64_t v = 0;
    for (int i = 0; i < n; ++i) v = (v << 8) | s[off + i];
    return v;
}

std::uint32_t getLe32(std::span<const std::uint8_t> s, std::size_t off) {
    return s[off] | (s[off + 1] << 8) | (s[off + 2] << 16) | (static_cast<std::uint32_t>(s[off + 3]) << 24);
}

} // namespace

Writer& Writer::item(std::uint8_t type, std::span<const std::uint8_t> value) {
    buf_.push_back(type);
    putBe(buf_, value.size(), 4);
    buf_.insert(buf_.end(), value.begin(), value.end());
    return *this;
}

Writer& Writer::u32(std::uint32_t v) {
    buf_.push_back(U32);
    putBe(buf_, 4, 4);
    putBe(buf_, v, 4);
    return *this;
}

Writer& Writer::fixed(double v) {
    buf_.push_back(Fixed42);
    putBe(buf_, 8, 4);
    putBe(buf_, static_cast<std::uint64_t>(std::llround(std::ldexp(v, 42))), 8);
    return *this;
}

Writer& Writer::structHeader(std::uint16_t count, std::uint16_t id) {
    buf_.push_back(Struct);
    putBe(buf_, 4, 4);
    putBe(buf_, count, 2);
    putBe(buf_, id, 2);
    return *this;
}

Writer& Writer::point3(double x, double y, double z) {
    return structHeader(3, kPoint3).fixed(x).fixed(y).fixed(z);
}

Writer& Writer::blob(std::span<const std::uint8_t> data) {
    buf_.push_back(Blob);
    putBe(buf_, data.size() + 4, 4);
    putBe(buf_, data.size(), 4);
    buf_.insert(buf_.end(), data.begin(), data.end());
    return *this;
}

std::uint64_t Item::asUint() const {
    return value.size() <= 8 ? getBe(value, 0, static_cast<int>(value.size())) : 0;
}

double Item::asFixed() const {
    if (type == Fixed42 && value.size() == 8)
        return std::ldexp(static_cast<double>(static_cast<std::int64_t>(getBe(value, 0, 8))), -42);
    if (type == Fixed16 && value.size() == 4)
        return std::ldexp(static_cast<double>(getBe(value, 0, 4)), -16);
    return 0.0;
}

std::span<const std::uint8_t> Item::asData() const {
    if (value.size() < 4) return {};
    std::size_t n = getBe(value, 0, 4);
    return n <= value.size() - 4 ? value.subspan(4, n) : std::span<const std::uint8_t>{};
}

std::vector<Item> parseItems(std::span<const std::uint8_t> p) {
    std::vector<Item> out;
    std::size_t i = 2;
    while (i + 5 <= p.size()) {
        std::size_t len = getBe(p, i + 1, 4);
        if (len > p.size() - i - 5) break;
        out.push_back({p[i], p.subspan(i + 5, len)});
        i += 5 + len;
    }
    return out;
}

std::vector<Bytes> encodeRequest(std::uint32_t seq, std::uint32_t op, std::span<const std::uint8_t> payload) {
    Bytes msg;
    msg.reserve(kHeader + payload.size());
    for (std::uint32_t v : {static_cast<std::uint32_t>(Request), seq, 0u, op, 0u,
                            static_cast<std::uint32_t>(payload.size())})
        putBe(msg, v, 4);
    msg.insert(msg.end(), payload.begin(), payload.end());

    // Each packet: envelope (kind 0, bytes that follow), then a slice of the message.
    std::vector<Bytes> packets;
    for (std::size_t off = 0; off < msg.size(); off += kMaxPacket - kEnvelope) {
        std::size_t n = std::min(kMaxPacket - kEnvelope, msg.size() - off);
        Bytes& p = packets.emplace_back();
        p.reserve(kEnvelope + n);
        putLe32(p, 0);
        putLe32(p, static_cast<std::uint32_t>(n));
        p.insert(p.end(), msg.begin() + static_cast<std::ptrdiff_t>(off), msg.begin() + static_cast<std::ptrdiff_t>(off + n));
    }
    return packets;
}

void FrameParser::feed(std::span<const std::uint8_t> data) {
    buf_.insert(buf_.end(), data.begin(), data.end());
}

std::optional<Message> FrameParser::next() {
    auto fail = [this]() -> std::optional<Message> {
        buf_.clear();
        partial_.reset();
        throw std::runtime_error("TTP stream out of sync");
    };

    // Device-to-host envelope: kind 1, then the envelope's total length including
    // itself. The first envelope of a message holds the header and the start of
    // the payload; if the payload is longer, it continues in further envelopes.
    for (;;) {
        if (buf_.size() < kEnvelope) return std::nullopt;
        std::size_t total = getLe32(buf_, 4);
        std::size_t minimum = kEnvelope + (partial_ ? 1 : kHeader);
        if (getLe32(buf_, 0) != 1 || total < minimum || total > kMaxMessage) return fail();
        if (buf_.size() < total) return std::nullopt;

        std::span<const std::uint8_t> body(buf_.data() + kEnvelope, total - kEnvelope);
        if (!partial_) {
            Message m;
            m.magic = static_cast<std::uint32_t>(getBe(body, 0, 4));
            m.seq = static_cast<std::uint32_t>(getBe(body, 4, 4));
            m.op = static_cast<std::uint32_t>(getBe(body, 12, 4));
            m.status = static_cast<std::uint32_t>(getBe(body, 16, 4));
            expected_ = getBe(body, 20, 4);
            if (expected_ > kMaxMessage) return fail();
            body = body.subspan(kHeader);
            partial_ = std::move(m);
            partial_->payload.reserve(expected_);
        }
        if (partial_->payload.size() + body.size() > expected_) return fail();
        partial_->payload.insert(partial_->payload.end(), body.begin(), body.end());
        buf_.erase(buf_.begin(), buf_.begin() + static_cast<std::ptrdiff_t>(total));

        if (partial_->payload.size() == expected_) {
            Message done = std::move(*partial_);
            partial_.reset();
            return done;
        }
    }
}

} // namespace eyenix::ttp
