#pragma once

#include "MessageDefs.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace MessageManager
{
namespace detail
{
constexpr size_t kLengthPrefixSize = IPC_LENGTH_PREFIX_SIZE;

// Decoded view of ipc_frame. This is not the wire layout; the wire body is
// protobuf serialized (see proto/ipc_frame.proto).
struct IpcHeader
{
    uint8_t magic[4] = {0, 0, 0, 0};
    uint8_t ver = 0;
    uint8_t type = 0;
    uint16_t cmd = 0;
    uint16_t seq_id = 0;
    uint8_t flag = 0;
    uint8_t code = 0;
};

struct Frame
{
    IpcHeader hdr{};
    Payload payload;
};

std::vector<uint8_t> EncodeFrame(const IpcHeader& hdr, const Payload& payload);
std::vector<uint8_t> EncodeFrame(const IpcHeader& hdr, const std::string& text);

enum class ParsedFrame
{
    OK,
    NEED_MORE,
    ERROR,
};

// Incremental parser:
//   [u32 size little-endian][serialized ipc_frame]...
// Only emits a frame once the full protobuf body is available.
class FrameParser
{
public:
    FrameParser(std::array<uint8_t, 4> magic, uint8_t version,
                uint32_t max_payload);

    void append(const uint8_t* data, size_t len);
    void clear();

    ParsedFrame next(Frame& out);

    ErrorCode error_code() const
    {
        return _error_code;
    }
    const char* error_text() const
    {
        return _error_text;
    }

    bool has_header() const
    {
        return _has_header;
    }
    const IpcHeader& last_header() const
    {
        return _last_header;
    }

private:
    size_t available() const
    {
        return _buf.size() - _off;
    }
    void compact();
    ParsedFrame fail(ErrorCode code, const char* text);

    std::array<uint8_t, 4> _magic;
    uint8_t _version = 1;
    uint32_t _max_payload = static_cast<uint32_t>(IPC_MAX_PAYLOAD);

    std::vector<uint8_t> _buf;
    size_t _off = 0;

    ErrorCode _error_code = ErrorCode::OK;
    const char* _error_text = "";
    bool _has_header = false;
    IpcHeader _last_header{};
};

} // namespace detail
} // namespace MessageManager
