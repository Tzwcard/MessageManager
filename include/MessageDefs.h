#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace MessageManager
{
// ---------------------------------------------------------------------------
// wire 常量（见 IPC_FRAMING.md）
// ---------------------------------------------------------------------------

constexpr size_t IPC_HEADER_SIZE = 16;
constexpr size_t IPC_MAX_PAYLOAD = 1024 * 1024; // 1 MiB

constexpr uint8_t IPC_FLAG_NO_RESPONSE = 0x01;
constexpr uint8_t IPC_FLAG_ERROR = 0x02;

constexpr uint16_t IPC_SEQ_CALL_MIN = 0x0001;
constexpr uint16_t IPC_SEQ_CALL_MAX = 0x7FFF;
constexpr uint16_t IPC_SEQ_NOTIFY_MIN = 0x8001;
constexpr uint16_t IPC_SEQ_NOTIFY_MAX = 0xFFFF;
constexpr uint16_t IPC_SEQ_NOTIFY_MASK = 0x8000;

// framing 层 general error 保留 command。
// profile 不得把 0x0000 分配给业务命令。
constexpr uint16_t IPC_CMD_GENERAL_ERROR = 0x0000;

using MessageId = uint64_t;
using Payload = std::vector<uint8_t>;

enum class MessageType : uint8_t
{
    Call = 0,
    Response = 1,
    Notify = 2,
};

enum class SendMode
{
    Request, // 需要 response，进入 pending map
    OneWay, // 不需要 response，不进入 pending map
};

enum class MMMode
{
    Client,
    Server,
};

// wire 的 code 字段：flag 带 IPC_FLAG_ERROR 时是 error code，否则为 0。
enum class ErrorCode : uint8_t
{
    OK = 0,
    INVALID_ARGUMENT = 1,
    UNAVAILABLE = 2,
    PERMISSION_DENIED = 3,
    FAILED = 4,
    TIMEOUT = 5,
    NOT_FOUND = 6,
    ALREADY_RUNNING = 7,
    NOT_RUNNING = 8,
    BUSY = 9,
    NOT_SUPPORTED = 10,
    UNSUPPORTED_VERSION = 11,
    MAGIC_MISMATCH = 12,
};

// 本地 API 返回值（非 wire 字段）。
// recv: >0 = id, 0 = timeout, <0 = 错误（AGAIN / NOT_PENDING / CLOSED / INVALID）。
// fetch_*: >0 = id, 0 = 队列空, <0 = 错误。
enum class Status : int
{
    OK = 0,
    NOT_SUPPORTED = -1,
    INVALID = -2,
    NOT_PENDING = -3,
    CLOSED = -5,
    NOT_FOUND = -6,
    BUSY = -7,
    AGAIN = -8,
    TRANSPORT = -9,
};

// set_disconnect_callback 的 reason。
enum class DisconnectReason : int
{
    PEER_CLOSED = 1,
    TRANSPORT_ERROR = 2,
    PROTOCOL_ERROR = 3,
    QUEUE_OVERFLOW = 4,
    SHUTDOWN = 5,
};

struct Message
{
    MessageId id = 0; // MM 本地 id（当前实现等于 seq_id）
    MessageType type = MessageType::Call;
    uint16_t cmd = 0;
    uint8_t flag = 0; // IPC_FLAG_*
    uint8_t code = 0; // flag & IPC_FLAG_ERROR 时为 error code，否则 0
    uint16_t seq_id = 0;
    Payload payload;
};

struct MessageManagerConfig
{
    std::array<uint8_t, 4> magic = {0x20, 0x26, 0x11, 0x05};
    uint8_t version = 1;
    MMMode mode = MMMode::Server;
    uint32_t max_payload = static_cast<uint32_t>(IPC_MAX_PAYLOAD);
    size_t max_request_queue = 64;
    size_t max_notify_queue = 64;
    size_t max_outbound_queue = 256;
};

inline bool IsErrorFlag(uint8_t flag)
{
    return (flag & IPC_FLAG_ERROR) != 0;
}

inline bool IsNotifySeq(uint16_t seq_id)
{
    return (seq_id & IPC_SEQ_NOTIFY_MASK) != 0;
}

inline bool IsGeneralErrorCmd(uint16_t cmd)
{
    return cmd == IPC_CMD_GENERAL_ERROR;
}

} // namespace MessageManager
