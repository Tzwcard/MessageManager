#pragma once

#include "ITransport.h"
#include "MessageDefs.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace MessageManager
{
namespace detail
{
struct Frame;
struct IpcHeader;
} // namespace detail

// 一个 MessageManager 对应一个 transport（1:1），只服务一条连接。
//
//   Client : send / recv / fetch_notify / discard
//   Server : fetch_request / reply / reply_error / notify
//
// reader 线程只做 framing / 队列 / 匹配，不执行任何业务代码。
// payload 对 MM 是不透明字节。
//
// 生命周期：断开（对端 close / I/O 错误 / 协议错误）或调用 end() 之后，
// 实例进入终态：assign_transport() 返回 INVALID，start() 返回 CLOSED，
// 不能通过换 transport / 换 fd 复用。重连必须析构旧实例，用新的
// MessageManager 绑定新连接。这是 MM 自身的状态机，与 transport 实现无关。
class MessageManager
{
public:
    explicit MessageManager(MessageManagerConfig config);
    ~MessageManager();

    MessageManager(const MessageManager&) = delete;
    MessageManager& operator=(const MessageManager&) = delete;

    // 只能在 start() 之前调用一次。
    //
    // 拥有形式：MessageManager 负责销毁。
    int assign_transport(std::unique_ptr<ITransport> transport);

    // 非拥有形式：transport 必须比 MessageManager 活得久。
    // 适合栈上的 CUnixSocksTransport。
    int assign_transport(ITransport& transport);
    int assign_transport(ITransport* transport);

    int start();
    void end();

    bool running() const;

    // client: 发 Call 并入队，立即返回 MessageId (>0)，失败 <0。
    // timeout_ms 只对 Request 生效；OneWay 不进入 pending，直接忽略。
    int send(uint16_t cmd, const Payload& payload,
             SendMode mode = SendMode::Request, int timeout_ms = -1);

    // client: 非阻塞检查 pending，立即返回。
    // >0  = MessageId，并取走 response
    // 0   = send 设置的 deadline 已过，pending 已移除
    // -AGAIN      = 还没到，调用方下一轮 loop 再查
    // -NOT_PENDING / -CLOSED / -INVALID
    int recv(MessageId id, Message* out);

    // client: 非阻塞消费 Notify。
    // >0 = MessageId，0 = 队列空，<0 = CLOSED / INVALID / NOT_SUPPORTED。
    int fetch_notify(Message* out);

    // server: 非阻塞消费 Call。
    // >0 = MessageId，0 = 队列空，<0 = CLOSED / INVALID / NOT_SUPPORTED。
    int fetch_request(Message* out);

    // server: 回复 Call。flag 是 wire 的 flag 字节（IPC_FLAG_*）。
    int reply(MessageId call_id, uint16_t cmd, const Payload& payload,
              uint8_t flag = 0);

    // server: 错误回复，payload 为 UTF-8 纯文本。
    int reply_error(MessageId call_id, uint16_t cmd,
                    uint8_t error_code, const std::string& message);

    // server: 向唯一 client 推事件，无 ACK。
    int notify(uint16_t cmd, const Payload& payload);

    // client: 放弃 pending Request。之后 recv 返回 NOT_PENDING。
    int discard(MessageId id);

    // 连接断开 / 致命错误。reason 是 DisconnectReason 的 int 值。
    // 回调在 reader 线程里执行，不要在回调里调用 end()。
    int set_disconnect_callback(std::function<void(int reason)> callback);

private:
    struct Pending
    {
        bool ready = false; // response 已到，等待 recv
        bool has_deadline = false; // send 是否设置了 timeout
        std::chrono::steady_clock::time_point deadline;
        Message response;
    };

    struct Outbound
    {
        std::vector<uint8_t> bytes;
        size_t sent = 0;
    };

    static MessageManagerConfig Sanitize(MessageManagerConfig config);

    bool is_running() const;
    bool push_outbound(std::vector<uint8_t> frame);
    bool has_outbound() const;
    void flush_outbound();

    void reader_loop();
    void dispatch_frame(const detail::Frame& frame);
    void handle_protocol_error(const detail::IpcHeader& header,
                               ErrorCode code, const char* text);
    void disconnect(DisconnectReason reason);

    int enqueue_error_response(uint16_t seq_id, uint16_t cmd,
                               uint8_t error_code, const char* text);
    int reply_impl(MessageId call_id, uint16_t cmd, const Payload& payload,
                   uint8_t flag, uint8_t code);
    int fetch_from(std::deque<Message>& queue, Message* out);

    // 两个 alloc 都在 _pending_mutex 下调用。
    bool alloc_call_seq(uint16_t& seq);
    bool alloc_notify_seq(uint16_t& seq);

    MessageManagerConfig _config;

    mutable std::mutex _lifecycle_mutex;
    std::unique_ptr<ITransport> _owned_transport;
    ITransport* _transport = nullptr;
    std::thread _reader;
    bool _started = false; // guarded by _lifecycle_mutex
    std::atomic<bool> _running{false};
    std::atomic<bool> _closed{false};

    mutable std::mutex _outbound_mutex;
    std::deque<Outbound> _outbound;

    mutable std::mutex _incoming_mutex;
    std::deque<Message> _request_queue;
    std::deque<Message> _notify_queue;

    mutable std::mutex _pending_mutex;
    std::map<uint16_t, Pending> _pending;
    uint16_t _next_call_seq = IPC_SEQ_CALL_MIN;
    uint16_t _next_notify_seq = IPC_SEQ_NOTIFY_MIN;

    std::mutex _callback_mutex;
    std::function<void(int)> _disconnect_callback;
};

} // namespace MessageManager
