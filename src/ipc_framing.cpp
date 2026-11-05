#include "ipc_framing.h"

#include <cstring>

namespace MessageManager
{
namespace detail
{
namespace
{
void PutU16(uint8_t* out, uint16_t value)
{
    out[0] = static_cast<uint8_t>(value & 0xFF);
    out[1] = static_cast<uint8_t>((value >> 8) & 0xFF);
}

uint16_t GetU16(const uint8_t* in)
{
    return static_cast<uint16_t>(static_cast<uint16_t>(in[0]) |
                                 (static_cast<uint16_t>(in[1]) << 8));
}

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

void EncodeHeader(const IpcHeader& hdr, uint8_t out[IPC_HEADER_SIZE])
{
    std::memcpy(out, hdr.magic, 4);
    out[4] = hdr.ver;
    out[5] = hdr.type;
    PutU16(out + 6, hdr.cmd);
    PutU16(out + 8, hdr.seq_id);
    out[10] = hdr.flag;
    out[11] = hdr.code;
    PutU32(out + 12, hdr.sz_payload);
}

void DecodeHeader(const uint8_t in[IPC_HEADER_SIZE], IpcHeader& hdr)
{
    std::memcpy(hdr.magic, in, 4);
    hdr.ver = in[4];
    hdr.type = in[5];
    hdr.cmd = GetU16(in + 6);
    hdr.seq_id = GetU16(in + 8);
    hdr.flag = in[10];
    hdr.code = in[11];
    hdr.sz_payload = GetU32(in + 12);
}

std::vector<uint8_t> EncodeFrame(const IpcHeader& hdr, const Payload& payload)
{
    IpcHeader wire = hdr;
    wire.sz_payload = static_cast<uint32_t>(payload.size());

    std::vector<uint8_t> out(IPC_HEADER_SIZE + payload.size());
    EncodeHeader(wire, out.data());
    if (!payload.empty())
        std::memcpy(out.data() + IPC_HEADER_SIZE, payload.data(), payload.size());
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

    // 只有已消费的前缀足够大时才搬移，避免每帧都 erase。
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

    // 1. 读满 16 字节 header。
    if (available() < IPC_HEADER_SIZE)
        return ParsedFrame::NEED_MORE;

    IpcHeader hdr{};
    DecodeHeader(_buf.data() + _off, hdr);
    _last_header = hdr;
    _has_header = true;

    // 2. 结构校验。
    if (std::memcmp(hdr.magic, _magic.data(), 4) != 0)
        return fail(ErrorCode::MAGIC_MISMATCH, "magic mismatch");

    if (hdr.ver != _version)
        return fail(ErrorCode::UNSUPPORTED_VERSION, "unsupported version");

    if (hdr.type > static_cast<uint8_t>(MessageType::Notify))
        return fail(ErrorCode::INVALID_ARGUMENT, "invalid message type");

    if (hdr.seq_id == 0 || hdr.seq_id == IPC_SEQ_NOTIFY_MASK)
        return fail(ErrorCode::INVALID_ARGUMENT, "reserved seq_id");

    if ((hdr.type == static_cast<uint8_t>(MessageType::Notify)) !=
        IsNotifySeq(hdr.seq_id))
        return fail(ErrorCode::INVALID_ARGUMENT, "type/seq_id mismatch");

    // 0x0000 只用于 framing error 的 Response；
    // Call / Notify 不能把它当业务命令。
    if (hdr.type != static_cast<uint8_t>(MessageType::Response) &&
        IsGeneralErrorCmd(hdr.cmd))
        return fail(ErrorCode::INVALID_ARGUMENT, "reserved command");

    if (hdr.sz_payload > _max_payload || hdr.sz_payload > IPC_MAX_PAYLOAD)
        return fail(ErrorCode::INVALID_ARGUMENT, "payload too large");

    // 3. 等够整帧。
    const size_t frame_size = IPC_HEADER_SIZE + static_cast<size_t>(hdr.sz_payload);
    if (available() < frame_size)
        return ParsedFrame::NEED_MORE;

    // 4. 产出完整 frame。
    out.hdr = hdr;
    const uint8_t* begin = _buf.data() + _off + IPC_HEADER_SIZE;
    out.payload.assign(begin, begin + hdr.sz_payload);

    _off += frame_size;
    compact();
    return ParsedFrame::OK;
}

} // namespace detail
} // namespace MessageManager
