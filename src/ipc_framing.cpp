#include "ipc_framing.h"

#include "ipc_frame.pb.h"

#include <cstring>

namespace MessageManager
{
namespace detail
{
namespace
{
void PutU32(uint8_t* out, uint32_t value)
{
    out[0] = static_cast<uint8_t>(value & 0xFF);
    out[1] = static_cast<uint8_t>((value >> 8) & 0xFF);
    out[2] = static_cast<uint8_t>((value >> 16) & 0xFF);
    out[3] = static_cast<uint8_t>((value >> 24) & 0xFF);
}

uint32_t GetU32(const uint8_t* in)
{
    return static_cast<uint32_t>(in[0]) |
           (static_cast<uint32_t>(in[1]) << 8) |
           (static_cast<uint32_t>(in[2]) << 16) |
           (static_cast<uint32_t>(in[3]) << 24);
}
} // namespace

std::vector<uint8_t> EncodeFrame(const IpcHeader& hdr, const Payload& payload)
{
    msgmgr::ipc::ipc_frame wire;
    wire.set_magic(hdr.magic, 4);
    wire.set_ver(hdr.ver);
    wire.set_type(hdr.type);
    wire.set_command(hdr.cmd);
    wire.set_seq_id(hdr.seq_id);
    wire.set_flag(hdr.flag);
    wire.set_code(hdr.code);

    if (!payload.empty())
        wire.set_payload(payload.data(), payload.size());

    std::string body;
    if (!wire.SerializeToString(&body))
        return {};

    if (body.size() > IPC_MAX_FRAME_SIZE)
        return {};

    std::vector<uint8_t> out(kLengthPrefixSize + body.size());
    PutU32(out.data(), static_cast<uint32_t>(body.size()));
    std::memcpy(out.data() + kLengthPrefixSize, body.data(), body.size());
    return out;
}

std::vector<uint8_t> EncodeFrame(const IpcHeader& hdr, const std::string& text)
{
    const Payload payload(text.begin(), text.end());
    return EncodeFrame(hdr, payload);
}

FrameParser::FrameParser(std::array<uint8_t, 4> magic, uint8_t version,
                         uint32_t max_payload)
    : _magic(magic), _version(version), _max_payload(max_payload)
{
}

void FrameParser::append(const uint8_t* data, size_t len)
{
    if (!data || len == 0)
        return;
    _buf.insert(_buf.end(), data, data + len);
}

void FrameParser::clear()
{
    _buf.clear();
    _off = 0;
    _error_code = ErrorCode::OK;
    _error_text = "";
    _has_header = false;
    _last_header = IpcHeader{};
}

void FrameParser::compact()
{
    if (_off == 0)
        return;

    if (_off == _buf.size())
    {
        _buf.clear();
        _off = 0;
        return;
    }

    if (_off >= 64 * 1024 && _off * 2 >= _buf.size())
    {
        _buf.erase(_buf.begin(), _buf.begin() + static_cast<std::ptrdiff_t>(_off));
        _off = 0;
    }
}

ParsedFrame FrameParser::fail(ErrorCode code, const char* text)
{
    _error_code = code;
    _error_text = text;
    return ParsedFrame::ERROR;
}

ParsedFrame FrameParser::next(Frame& out)
{
    _has_header = false;

    if (available() < kLengthPrefixSize)
        return ParsedFrame::NEED_MORE;

    const uint32_t size = GetU32(_buf.data() + _off);
    if (size == 0 || size > IPC_MAX_FRAME_SIZE)
        return fail(ErrorCode::INVALID_ARGUMENT, "invalid frame size");

    if (available() < kLengthPrefixSize + size)
        return ParsedFrame::NEED_MORE;

    msgmgr::ipc::ipc_frame wire;
    const uint8_t* body = _buf.data() + _off + kLengthPrefixSize;
    if (!wire.ParseFromArray(body, static_cast<int>(size)))
        return fail(ErrorCode::INVALID_ARGUMENT, "invalid protobuf frame");

    // Fill a best-effort header first; it is also used by error replies.
    if (wire.magic().size() == 4)
    {
        std::memcpy(_last_header.magic, wire.magic().data(), 4);
    }
    _last_header.ver = static_cast<uint8_t>(wire.ver());
    _last_header.type = static_cast<uint8_t>(wire.type());
    _last_header.cmd = static_cast<uint16_t>(wire.command());
    _last_header.seq_id = static_cast<uint16_t>(wire.seq_id());
    _last_header.flag = static_cast<uint8_t>(wire.flag());
    _last_header.code = static_cast<uint8_t>(wire.code());
    _has_header = true;

    if (wire.ver() > 0xFF || wire.type() > 0xFF ||
        wire.flag() > 0xFF || wire.code() > 0xFF ||
        wire.command() > 0xFFFF || wire.seq_id() > 0xFFFF)
    {
        return fail(ErrorCode::INVALID_ARGUMENT, "field out of range");
    }

    if (wire.magic().size() != 4)
        return fail(ErrorCode::INVALID_ARGUMENT, "invalid magic length");

    if (std::memcmp(wire.magic().data(), _magic.data(), 4) != 0)
        return fail(ErrorCode::MAGIC_MISMATCH, "magic mismatch");

    if (wire.ver() != _version)
        return fail(ErrorCode::UNSUPPORTED_VERSION, "unsupported version");

    if (wire.type() > static_cast<uint32_t>(MessageType::Notify))
        return fail(ErrorCode::INVALID_ARGUMENT, "invalid message type");

    if (wire.seq_id() == 0 || wire.seq_id() == IPC_SEQ_NOTIFY_MASK)
        return fail(ErrorCode::INVALID_ARGUMENT, "reserved seq_id");

    if ((wire.type() == static_cast<uint32_t>(MessageType::Notify)) !=
        IsNotifySeq(static_cast<uint16_t>(wire.seq_id())))
        return fail(ErrorCode::INVALID_ARGUMENT, "type/seq_id mismatch");

    if (wire.type() != static_cast<uint32_t>(MessageType::Response) &&
        IsGeneralErrorCmd(static_cast<uint16_t>(wire.command())))
        return fail(ErrorCode::INVALID_ARGUMENT, "reserved command");

    if (wire.payload().size() > _max_payload ||
        wire.payload().size() > IPC_MAX_PAYLOAD)
        return fail(ErrorCode::INVALID_ARGUMENT, "payload too large");

    out.hdr = _last_header;
    out.payload.assign(wire.payload().begin(), wire.payload().end());

    _off += kLengthPrefixSize + size;
    compact();
    return ParsedFrame::OK;
}

} // namespace detail
} // namespace MessageManager
