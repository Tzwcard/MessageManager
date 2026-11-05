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
// wire header：固定 16 字节、little-endian。
// 不要直接把 read() 的临时 buffer 强转成这个结构；统一走 FrameParser。
#pragma pack(push, 1)
struct IpcHeader
{
    uint8_t magic[4];
    uint8_t ver;
    uint8_t type;
    uint16_t cmd;
    uint16_t seq_id;
    uint8_t flag;
    uint8_t code;
    uint32_t sz_payload;
};
#pragma pack(pop)

static_assert(sizeof(IpcHeader) == IPC_HEADER_SIZE, "IPC header must be 16 bytes");

// 一帧完整消息。header + 不透明 payload。
struct Frame
{
    IpcHeader hdr{};
    Payload payload;
};

// 显式按 little-endian 编码 / 解码，不依赖主机字节序。
void EncodeHeader(const IpcHeader& hdr, uint8_t out[IPC_HEADER_SIZE]);
void DecodeHeader(const uint8_t in[IPC_HEADER_SIZE], IpcHeader& hdr);

std::vector<uint8_t> EncodeFrame(const IpcHeader& hdr, const Payload& payload);
std::vector<uint8_t> EncodeFrame(const IpcHeader& hdr, const std::string& text);

enum class ParsedFrame
{
    OK, // out 里是一整帧
    NEED_MORE, // 数据还不够
    ERROR, // 协议错误，调用方必须关闭连接
};

// 增量 parser：
//   - 一次 read 可能只有半个 header、header + 部分 payload，或包含多帧；
//   - 只有 available >= 16 + sz_payload 时才产出一帧；
//   - 校验 magic / ver / type / seq_id / sz_payload。
class FrameParser
{
public:
    FrameParser(std::array<uint8_t, 4> magic, uint8_t version, uint32_t max_payload);

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

    // 出错时 header 是否可读（用于回 error frame）。
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
