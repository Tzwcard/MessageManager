#include "MessageManager.h"

#include "ipc_framing.h"

#include <cerrno>
#include <chrono>
#include <cstring>
#include <string>
#include <system_error>
#include <utility>

namespace MessageManager
{
namespace
{
using Clock = std::chrono::steady_clock;

void FillHeader(detail::IpcHeader& hdr, const MessageManagerConfig& config,
                MessageType type, uint16_t cmd, uint16_t seq_id,
                uint8_t flag, uint8_t code)
{
    hdr = detail::IpcHeader{};
    std::memcpy(hdr.magic, config.magic.data(), 4);
    hdr.ver = config.version;
    hdr.type = static_cast<uint8_t>(type);
    hdr.cmd = cmd;
    hdr.seq_id = seq_id;
    hdr.flag = flag;
    hdr.code = code;
}
} // namespace

MessageManager::MessageManager(MessageManagerConfig config)
    : _config(Sanitize(std::move(config)))
{
}

MessageManager::~MessageManager()
{
    end();
}

MessageManagerConfig MessageManager::Sanitize(MessageManagerConfig config)
{
    if (config.max_payload == 0 || config.max_payload > IPC_MAX_PAYLOAD)
        config.max_payload = static_cast<uint32_t>(IPC_MAX_PAYLOAD);
    if (config.max_request_queue == 0)
        config.max_request_queue = 1;
    if (config.max_notify_queue == 0)
        config.max_notify_queue = 1;
    if (config.max_outbound_queue == 0)
        config.max_outbound_queue = 1;
    return config;
}

int MessageManager::assign_transport(std::unique_ptr<ITransport> transport)
{
    if (!transport)
        return static_cast<int>(Status::INVALID);

    std::lock_guard<std::mutex> lock(_lifecycle_mutex);
    if (_started || _transport)
        return static_cast<int>(Status::INVALID);

    _owned_transport = std::move(transport);
    _transport = _owned_transport.get();
    return 0;
}

int MessageManager::assign_transport(ITransport& transport)
{
    std::lock_guard<std::mutex> lock(_lifecycle_mutex);
    if (_started || _transport)
        return static_cast<int>(Status::INVALID);

    _transport = &transport;
    return 0;
}

int MessageManager::assign_transport(ITransport* transport)
{
    if (!transport)
        return static_cast<int>(Status::INVALID);
    return assign_transport(*transport);
}

int MessageManager::start()
{
    std::lock_guard<std::mutex> lock(_lifecycle_mutex);
    if (_started)
        return static_cast<int>(Status::INVALID);
    if (!_transport || _closed.load())
        return static_cast<int>(Status::CLOSED);

    _running.store(true);
    try
    {
        _reader = std::thread(&MessageManager::reader_loop, this);
    }
    catch (const std::system_error&)
    {
        _running.store(false);
        return static_cast<int>(Status::INVALID);
    }

    _started = true;
    return 0;
}

void MessageManager::end()
{
    std::thread reader;
    bool was_closed = false;
    {
        std::lock_guard<std::mutex> lock(_lifecycle_mutex);
        _started = false;
        _running.store(false);
        was_closed = _closed.exchange(true);
        // 已经关闭过（disconnect 或上一次 end）就不要再碰 transport；
        // 非拥有 transport 可能已经比 MM 先销毁。
        if (!was_closed && _transport)
            _transport->shutdown();
        reader = std::move(_reader);
    }

    if (reader.joinable())
    {
        if (reader.get_id() == std::this_thread::get_id())
            reader.detach();
        else
            reader.join();
    }

    if (!was_closed)
    {
        std::lock_guard<std::mutex> lock(_lifecycle_mutex);
        if (_transport)
            _transport->close();
    }
}

bool MessageManager::is_running() const
{
    return _running.load() && !_closed.load() && _transport != nullptr;
}

bool MessageManager::running() const
{
    return is_running();
}

// ---------------------------------------------------------------------------
// client API
// ---------------------------------------------------------------------------

int MessageManager::send(uint16_t cmd, const Payload& payload, SendMode mode,
                         int timeout_ms)
{
    if (_config.mode != MMMode::Client)
        return static_cast<int>(Status::NOT_SUPPORTED);
    if (!is_running())
        return static_cast<int>(Status::CLOSED);
    if (payload.size() > _config.max_payload)
        return static_cast<int>(Status::INVALID);

    uint16_t seq = 0;
    {
        std::lock_guard<std::mutex> lock(_pending_mutex);
        if (!alloc_call_seq(seq))
            return static_cast<int>(Status::BUSY);

        if (mode == SendMode::Request)
        {
            Pending pending;
            pending.has_deadline = timeout_ms >= 0;
            if (pending.has_deadline)
                pending.deadline = Clock::now() +
                                   std::chrono::milliseconds(timeout_ms);
            _pending.emplace(seq, std::move(pending));
        }
    }

    detail::IpcHeader hdr;
    FillHeader(hdr, _config, MessageType::Call, cmd, seq,
               mode == SendMode::OneWay ? IPC_FLAG_NO_RESPONSE : 0, 0);

    if (!push_outbound(detail::EncodeFrame(hdr, payload)))
    {
        if (mode == SendMode::Request)
        {
            std::lock_guard<std::mutex> lock(_pending_mutex);
            _pending.erase(seq);
        }
        return static_cast<int>(_closed.load() ? Status::CLOSED : Status::BUSY);
    }

    return static_cast<int>(seq);
}

int MessageManager::recv(MessageId id, Message* out)
{
    if (_config.mode != MMMode::Client)
        return static_cast<int>(Status::NOT_SUPPORTED);
    if (!out)
        return static_cast<int>(Status::INVALID);
    if (id == 0 || id > IPC_SEQ_CALL_MAX)
        return static_cast<int>(Status::INVALID);

    const uint16_t seq = static_cast<uint16_t>(id);
    std::lock_guard<std::mutex> lock(_pending_mutex);

    auto it = _pending.find(seq);
    if (it == _pending.end())
        return static_cast<int>(Status::NOT_PENDING);

    if (it->second.ready)
    {
        *out = std::move(it->second.response);
        _pending.erase(it);
        return static_cast<int>(seq);
    }

    if (_closed.load())
    {
        _pending.erase(it);
        return static_cast<int>(Status::CLOSED);
    }

    if (it->second.has_deadline && Clock::now() >= it->second.deadline)
    {
        _pending.erase(it);
        return 0;
    }

    return static_cast<int>(Status::AGAIN);
}

int MessageManager::fetch_notify(Message* out)
{
    if (_config.mode != MMMode::Client)
        return static_cast<int>(Status::NOT_SUPPORTED);
    if (!out)
        return static_cast<int>(Status::INVALID);
    return fetch_from(_notify_queue, out);
}

int MessageManager::discard(MessageId id)
{
    if (_config.mode != MMMode::Client)
        return static_cast<int>(Status::NOT_SUPPORTED);
    if (id == 0 || id > IPC_SEQ_CALL_MAX)
        return static_cast<int>(Status::NOT_FOUND);

    const uint16_t seq = static_cast<uint16_t>(id);
    std::lock_guard<std::mutex> lock(_pending_mutex);

    auto it = _pending.find(seq);
    if (it == _pending.end())
        return static_cast<int>(Status::NOT_FOUND);

    _pending.erase(it);
    return 0;
}

// ---------------------------------------------------------------------------
// server API
// ---------------------------------------------------------------------------

int MessageManager::fetch_request(Message* out)
{
    if (_config.mode != MMMode::Server)
        return static_cast<int>(Status::NOT_SUPPORTED);
    if (!out)
        return static_cast<int>(Status::INVALID);
    return fetch_from(_request_queue, out);
}

int MessageManager::reply(MessageId call_id, uint16_t cmd,
                          const Payload& payload, uint8_t flag)
{
    return reply_impl(call_id, cmd, payload, flag, 0);
}

int MessageManager::reply_impl(MessageId call_id, uint16_t cmd,
                               const Payload& payload, uint8_t flag,
                               uint8_t code)
{
    if (_config.mode != MMMode::Server)
        return static_cast<int>(Status::NOT_SUPPORTED);
    if (!is_running())
        return static_cast<int>(Status::CLOSED);
    if (call_id == 0 || call_id > IPC_SEQ_CALL_MAX)
        return static_cast<int>(Status::INVALID);
    if (payload.size() > _config.max_payload)
        return static_cast<int>(Status::INVALID);

    detail::IpcHeader hdr;
    FillHeader(hdr, _config, MessageType::Response, cmd,
               static_cast<uint16_t>(call_id), flag, code);

    if (!push_outbound(detail::EncodeFrame(hdr, payload)))
        return static_cast<int>(_closed.load() ? Status::CLOSED : Status::BUSY);
    return 0;
}

int MessageManager::reply_error(MessageId call_id, uint16_t cmd,
                                uint8_t error_code, const std::string& message)
{
    if (_config.mode != MMMode::Server)
        return static_cast<int>(Status::NOT_SUPPORTED);

    const Payload payload(message.begin(), message.end());
    return reply_impl(call_id, cmd, payload, IPC_FLAG_ERROR, error_code);
}

int MessageManager::notify(uint16_t cmd, const Payload& payload)
{
    if (_config.mode != MMMode::Server)
        return static_cast<int>(Status::NOT_SUPPORTED);
    if (!is_running())
        return static_cast<int>(Status::CLOSED);
    if (payload.size() > _config.max_payload)
        return static_cast<int>(Status::INVALID);

    uint16_t seq = 0;
    {
        std::lock_guard<std::mutex> lock(_pending_mutex);
        alloc_notify_seq(seq);
    }

    detail::IpcHeader hdr;
    FillHeader(hdr, _config, MessageType::Notify, cmd, seq, 0, 0);

    if (!push_outbound(detail::EncodeFrame(hdr, payload)))
        return static_cast<int>(_closed.load() ? Status::CLOSED : Status::BUSY);
    return 0;
}

int MessageManager::set_disconnect_callback(std::function<void(int reason)> callback)
{
    std::lock_guard<std::mutex> lock(_callback_mutex);
    _disconnect_callback = std::move(callback);
    return 0;
}

// ---------------------------------------------------------------------------
// reader / queues
// ---------------------------------------------------------------------------

int MessageManager::fetch_from(std::deque<Message>& queue, Message* out)
{
    std::lock_guard<std::mutex> lock(_incoming_mutex);

    if (!queue.empty())
    {
        *out = std::move(queue.front());
        queue.pop_front();
        return static_cast<int>(out->id);
    }

    if (_closed.load())
        return static_cast<int>(Status::CLOSED);

    return 0;
}

bool MessageManager::push_outbound(std::vector<uint8_t> frame)
{
    {
        std::lock_guard<std::mutex> lock(_outbound_mutex);
        if (_closed.load())
            return false;
        if (_outbound.size() >= _config.max_outbound_queue)
            return false;

        Outbound outbound;
        outbound.bytes = std::move(frame);
        _outbound.push_back(std::move(outbound));
    }

    if (_transport)
        _transport->wake();
    return true;
}

bool MessageManager::has_outbound() const
{
    std::lock_guard<std::mutex> lock(_outbound_mutex);
    return !_outbound.empty();
}

void MessageManager::flush_outbound()
{
    for (;;)
    {
        bool failed = false;

        {
            std::lock_guard<std::mutex> lock(_outbound_mutex);
            if (_closed.load() || _outbound.empty())
                return;

            Outbound& front = _outbound.front();
            const size_t remain = front.bytes.size() - front.sent;

            const int written = _transport->write(front.bytes.data() + front.sent,
                                                  remain);
            if (written > 0)
            {
                front.sent += static_cast<size_t>(written);
                if (front.sent >= front.bytes.size())
                    _outbound.pop_front();
            }
            else if (written == -EAGAIN)
            {
                return;
            }
            else
            {
                failed = true;
            }
        }

        if (failed)
        {
            disconnect(DisconnectReason::TRANSPORT_ERROR);
            return;
        }
    }
}

void MessageManager::reader_loop()
{
    detail::FrameParser parser(_config.magic, _config.version,
                               _config.max_payload);
    std::vector<uint8_t> buffer(64 * 1024);

    while (_running.load())
    {
        const bool want_write = has_outbound();
        const int ready = _transport->wait(want_write, 200);

        if (!_running.load())
            break;

        if (ready < 0)
        {
            disconnect(DisconnectReason::TRANSPORT_ERROR);
            return;
        }

        if (ready & 1)
        {
            bool eof = false;
            bool io_error = false;

            // 一次 read 可能不够；读到 EAGAIN 或 EOF 为止，最多 16 轮，
            // 避免一直读把写队列饿死。
            for (int i = 0; i < 16; ++i)
            {
                const int n = _transport->read(buffer.data(), buffer.size());
                if (n > 0)
                {
                    parser.append(buffer.data(), static_cast<size_t>(n));
                    continue;
                }
                if (n == 0)
                {
                    eof = true;
                    break;
                }
                if (n == -EAGAIN)
                    break;

                io_error = true;
                break;
            }

            if (eof)
            {
                disconnect(DisconnectReason::PEER_CLOSED);
                return;
            }
            if (io_error)
            {
                disconnect(DisconnectReason::TRANSPORT_ERROR);
                return;
            }

            for (;;)
            {
                detail::Frame frame;
                const detail::ParsedFrame parsed = parser.next(frame);

                if (parsed == detail::ParsedFrame::NEED_MORE)
                    break;

                if (parsed == detail::ParsedFrame::ERROR)
                {
                    handle_protocol_error(parser.last_header(),
                                          parser.error_code(),
                                          parser.error_text());
                    return;
                }

                dispatch_frame(frame);

                if (!_running.load() || _closed.load())
                    return;
            }
        }

        flush_outbound();

        if (_closed.load())
            return;
    }
}

void MessageManager::dispatch_frame(const detail::Frame& frame)
{
    const uint8_t type = frame.hdr.type;
    const uint16_t seq = frame.hdr.seq_id;

    if (_config.mode == MMMode::Client)
    {
        if (type == static_cast<uint8_t>(MessageType::Response))
        {
            std::lock_guard<std::mutex> lock(_pending_mutex);
            auto it = _pending.find(seq);
            if (it == _pending.end())
                return;

            Message& response = it->second.response;
            response.id = seq;
            response.type = MessageType::Response;
            response.cmd = frame.hdr.cmd;
            response.flag = frame.hdr.flag;
            response.code = frame.hdr.code;
            response.seq_id = seq;
            response.payload = frame.payload;

            it->second.ready = true;
            return;
        }

        if (type == static_cast<uint8_t>(MessageType::Notify))
        {
            Message message;
            message.id = seq;
            message.type = MessageType::Notify;
            message.cmd = frame.hdr.cmd;
            message.flag = frame.hdr.flag;
            message.code = frame.hdr.code;
            message.seq_id = seq;
            message.payload = frame.payload;

            {
                std::lock_guard<std::mutex> lock(_incoming_mutex);
                if (_notify_queue.size() >= _config.max_notify_queue)
                    _notify_queue.pop_front(); // 当前策略：丢弃最旧的
                _notify_queue.push_back(std::move(message));
            }
            return;
        }

        // client 不该收到 Call。
        return;
    }

    if (type == static_cast<uint8_t>(MessageType::Call))
    {
        Message message;
        message.id = seq;
        message.type = MessageType::Call;
        message.cmd = frame.hdr.cmd;
        message.flag = frame.hdr.flag;
        message.code = frame.hdr.code;
        message.seq_id = seq;
        message.payload = frame.payload;

        bool queued = false;
        {
            std::lock_guard<std::mutex> lock(_incoming_mutex);
            if (_request_queue.size() < _config.max_request_queue)
            {
                _request_queue.push_back(std::move(message));
                queued = true;
            }
        }

        if (queued)
            return;

        // 队列满：OneWay 直接丢，Request 尽量回 BUSY。
        if (frame.hdr.flag & IPC_FLAG_NO_RESPONSE)
            return;

        if (enqueue_error_response(seq, frame.hdr.cmd,
                                   static_cast<uint8_t>(ErrorCode::BUSY),
                                   "queue full") < 0)
        {
            disconnect(DisconnectReason::QUEUE_OVERFLOW);
        }
        return;
    }

    // server 不该收到 Response / Notify。
}

int MessageManager::enqueue_error_response(uint16_t seq_id, uint16_t cmd,
                                           uint8_t error_code, const char* text)
{
    if (seq_id == 0 || seq_id > IPC_SEQ_CALL_MAX)
        return static_cast<int>(Status::INVALID);

    detail::IpcHeader hdr;
    FillHeader(hdr, _config, MessageType::Response, cmd, seq_id,
               IPC_FLAG_ERROR, error_code);

    const std::string message = text ? text : "";
    if (!push_outbound(detail::EncodeFrame(hdr, message)))
        return static_cast<int>(Status::BUSY);
    return 0;
}

void MessageManager::handle_protocol_error(const detail::IpcHeader& header,
                                           ErrorCode code, const char* text)
{
    // 错误帧用接收方自己的 magic / ver；framing 层错误不信任请求的 cmd，
    // 统一用 general error command。
    detail::IpcHeader hdr;
    FillHeader(hdr, _config, MessageType::Response, IPC_CMD_GENERAL_ERROR,
               header.seq_id, IPC_FLAG_ERROR, static_cast<uint8_t>(code));

    const std::string message = text ? text : "protocol error";
    const std::vector<uint8_t> bytes = detail::EncodeFrame(hdr, message);

    // 关闭前尽力把它写出去。
    size_t sent = 0;
    int retries = 0;
    while (sent < bytes.size() && retries < 100 && _transport)
    {
        const int written = _transport->write(bytes.data() + sent,
                                              bytes.size() - sent);
        if (written > 0)
        {
            sent += static_cast<size_t>(written);
            retries = 0;
            continue;
        }
        if (written == -EAGAIN)
        {
            if (_transport->wait(true, 10) < 0)
                break;
            ++retries;
            continue;
        }
        break;
    }

    disconnect(DisconnectReason::PROTOCOL_ERROR);
}

void MessageManager::disconnect(DisconnectReason reason)
{
    bool expected = false;
    if (!_closed.compare_exchange_strong(expected, true))
        return;

    _running.store(false);

    {
        std::lock_guard<std::mutex> lock(_lifecycle_mutex);
        if (_transport)
            _transport->close();
    }

    std::function<void(int)> callback;
    {
        std::lock_guard<std::mutex> lock(_callback_mutex);
        callback = _disconnect_callback;
    }

    if (callback)
        callback(static_cast<int>(reason));
}

// ---------------------------------------------------------------------------
// seq allocation
// ---------------------------------------------------------------------------

bool MessageManager::alloc_call_seq(uint16_t& seq)
{
    for (uint32_t i = 0; i < static_cast<uint32_t>(IPC_SEQ_CALL_MAX); ++i)
    {
        const uint16_t candidate = _next_call_seq;
        _next_call_seq = (candidate >= IPC_SEQ_CALL_MAX)
                             ? IPC_SEQ_CALL_MIN
                             : static_cast<uint16_t>(candidate + 1);

        if (_pending.find(candidate) == _pending.end())
        {
            seq = candidate;
            return true;
        }
    }
    return false;
}

bool MessageManager::alloc_notify_seq(uint16_t& seq)
{
    seq = _next_notify_seq;
    _next_notify_seq = (seq >= IPC_SEQ_NOTIFY_MAX)
                           ? IPC_SEQ_NOTIFY_MIN
                           : static_cast<uint16_t>(seq + 1);
    return true;
}

} // namespace MessageManager
