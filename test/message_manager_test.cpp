// MessageManager self test.
//
// Build:
//   cmake -S . -B build
//   cmake --build build
//   ctest --test-dir build --output-on-failure
//
// 覆盖：
//   - framing：LE 编码、增量 parser、结构校验错误；
//   - request / response、reply_error、OneWay、discard；
//   - notify / fetch_notify；
//   - magic / version 不匹配时回 error 并关闭；
//   - request queue 满回 BUSY；
//   - 模式约束。

#include "MessageManager.h"
#include "UnixSocksTransport.h"
#include "UnixSocket.h"
#include "ipc_framing.h"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace MM = MessageManager;

namespace
{
std::atomic<int> g_failures{0};

#define CHECK(expr)                                                     \
    do                                                                  \
    {                                                                   \
        if (!(expr))                                                    \
        {                                                               \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
            ++g_failures;                                               \
        }                                                               \
    } while (0)

void SleepMs(int ms)
{
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

bool WaitFor(const std::atomic<bool>& flag, int timeout_ms)
{
    const int steps = timeout_ms / 10;
    for (int i = 0; i < steps; ++i)
    {
        if (flag.load())
            return true;
        SleepMs(10);
    }
    return flag.load();
}

// async recv: 轮询到非 AGAIN 为止。测试辅助，不是 API。
int RecvWait(MM::MessageManager& mgr, MM::MessageId id, MM::Message* out,
             int timeout_ms)
{
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeout_ms);
    for (;;)
    {
        const int rc = mgr.recv(id, out);
        if (rc != static_cast<int>(MM::Status::AGAIN))
            return rc;
        if (std::chrono::steady_clock::now() >= deadline)
            return static_cast<int>(MM::Status::AGAIN);
        SleepMs(1);
    }
}

int FetchRequestWait(MM::MessageManager& mgr, MM::Message* out, int timeout_ms)
{
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeout_ms);
    for (;;)
    {
        const int rc = mgr.fetch_request(out);
        if (rc != 0)
            return rc;
        if (std::chrono::steady_clock::now() >= deadline)
            return 0;
        SleepMs(1);
    }
}

int FetchNotifyWait(MM::MessageManager& mgr, MM::Message* out, int timeout_ms)
{
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeout_ms);
    for (;;)
    {
        const int rc = mgr.fetch_notify(out);
        if (rc != 0)
            return rc;
        if (std::chrono::steady_clock::now() >= deadline)
            return 0;
        SleepMs(1);
    }
}

struct TransportPair
{
    MM::CUnixSocksTransport server;
    MM::CUnixSocksTransport client;
    std::string path;

    bool connect()
    {
        path = MM::MakeTempUnixSocketPath("mm-test");
        if (path.empty())
            return false;

        return server.ListenSocks(path) == 0 &&
               client.ConnectSocks(path) == 0 &&
               server.AcceptSocks() == 0;
    }
};

MM::MessageManagerConfig MakeConfig(MM::MMMode mode,
                                    const char* magic = "TEST",
                                    uint8_t version = 1)
{
    MM::MessageManagerConfig config;
    std::memcpy(config.magic.data(), magic, 4);
    config.version = version;
    config.mode = mode;
    return config;
}

MM::MessageManagerConfig MakeStressConfig(MM::MMMode mode)
{
    MM::MessageManagerConfig config = MakeConfig(mode);
    config.max_request_queue = 16384;
    config.max_notify_queue = 16384;
    config.max_outbound_queue = 16384;
    return config;
}

MM::Payload MakeValuePayload(uint32_t value)
{
    MM::Payload payload(4);
    payload[0] = static_cast<uint8_t>(value & 0xFF);
    payload[1] = static_cast<uint8_t>((value >> 8) & 0xFF);
    payload[2] = static_cast<uint8_t>((value >> 16) & 0xFF);
    payload[3] = static_cast<uint8_t>((value >> 24) & 0xFF);
    return payload;
}

uint32_t PayloadValue(const MM::Payload& payload)
{
    if (payload.size() != 4)
        return 0xFFFFFFFFu;
    return static_cast<uint32_t>(payload[0]) |
           (static_cast<uint32_t>(payload[1]) << 8) |
           (static_cast<uint32_t>(payload[2]) << 16) |
           (static_cast<uint32_t>(payload[3]) << 24);
}

MM::detail::IpcHeader MakeHeader(const char* magic, uint8_t version,
                                 MM::MessageType type, uint16_t cmd,
                                 uint16_t seq_id, uint8_t flag, uint8_t code,
                                 uint32_t payload_size)
{
    MM::detail::IpcHeader hdr{};
    std::memcpy(hdr.magic, magic, 4);
    hdr.ver = version;
    hdr.type = static_cast<uint8_t>(type);
    hdr.cmd = cmd;
    hdr.seq_id = seq_id;
    hdr.flag = flag;
    hdr.code = code;
    hdr.sz_payload = payload_size;
    return hdr;
}

// ---------------------------------------------------------------------------
// framing
// ---------------------------------------------------------------------------

void TestDefaultConfig()
{
    const MM::MessageManagerConfig config;
    CHECK(config.magic[0] == 0x20);
    CHECK(config.magic[1] == 0x26);
    CHECK(config.magic[2] == 0x11);
    CHECK(config.magic[3] == 0x05);
}

void TestFramingEncoding()
{
    const MM::detail::IpcHeader hdr =
        MakeHeader("TEST", 7, MM::MessageType::Call, 0x1234, 0x0102, 0x04, 0x03, 2);

    uint8_t bytes[MM::IPC_HEADER_SIZE]{};
    MM::detail::EncodeHeader(hdr, bytes);

    CHECK(bytes[0] == 'T' && bytes[1] == 'E' && bytes[2] == 'S' && bytes[3] == 'T');
    CHECK(bytes[4] == 7);
    CHECK(bytes[5] == 0);
    CHECK(bytes[6] == 0x34 && bytes[7] == 0x12);
    CHECK(bytes[8] == 0x02 && bytes[9] == 0x01);
    CHECK(bytes[10] == 0x04 && bytes[11] == 0x03);
    CHECK(bytes[12] == 2 && bytes[13] == 0 && bytes[14] == 0 && bytes[15] == 0);

    MM::detail::IpcHeader decoded{};
    MM::detail::DecodeHeader(bytes, decoded);
    CHECK(std::memcmp(decoded.magic, "TEST", 4) == 0);
    CHECK(decoded.ver == 7);
    CHECK(decoded.type == 0);
    CHECK(decoded.cmd == 0x1234);
    CHECK(decoded.seq_id == 0x0102);
    CHECK(decoded.flag == 0x04);
    CHECK(decoded.code == 0x03);
    CHECK(decoded.sz_payload == 2);

    const MM::Payload payload = {0x00, 0xFF, 0x10};
    const std::vector<uint8_t> frame =
        MM::detail::EncodeFrame(MakeHeader("TEST", 1, MM::MessageType::Call,
                                           0x0301, 1, 0, 0, 0),
                                payload);
    CHECK(frame.size() == MM::IPC_HEADER_SIZE + payload.size());
}

void TestFramingIncremental()
{
    MM::detail::FrameParser parser({'T', 'E', 'S', 'T'}, 1,
                                   static_cast<uint32_t>(MM::IPC_MAX_PAYLOAD));

    const MM::Payload payload1 = {1, 2, 3};
    const std::vector<uint8_t> frame1 =
        MM::detail::EncodeFrame(MakeHeader("TEST", 1, MM::MessageType::Call,
                                           0x0011, 1, 0, 0, 0),
                                payload1);
    const std::vector<uint8_t> frame2 =
        MM::detail::EncodeFrame(MakeHeader("TEST", 1, MM::MessageType::Notify,
                                           0x0022, 0x8001, 0, 0, 0),
                                MM::Payload{});

    std::vector<uint8_t> all = frame1;
    all.insert(all.end(), frame2.begin(), frame2.end());

    int produced = 0;
    for (uint8_t byte : all)
    {
        parser.append(&byte, 1);

        for (;;)
        {
            MM::detail::Frame frame;
            const MM::detail::ParsedFrame parsed = parser.next(frame);
            if (parsed == MM::detail::ParsedFrame::NEED_MORE)
                break;

            CHECK(parsed == MM::detail::ParsedFrame::OK);
            if (parsed != MM::detail::ParsedFrame::OK)
                return;

            ++produced;
            if (produced == 1)
            {
                CHECK(frame.hdr.cmd == 0x0011);
                CHECK(frame.hdr.seq_id == 1);
                CHECK(frame.payload == payload1);
            }
            else
            {
                CHECK(frame.hdr.type ==
                      static_cast<uint8_t>(MM::MessageType::Notify));
                CHECK(frame.hdr.seq_id == 0x8001);
                CHECK(frame.payload.empty());
            }
        }
    }

    CHECK(produced == 2);
}

void TestFramingErrors()
{
    struct Case
    {
        MM::detail::IpcHeader hdr;
        MM::ErrorCode expected;
    };

    const Case cases[] = {
        {MakeHeader("BAD!", 1, MM::MessageType::Call, 1, 1, 0, 0, 0),
         MM::ErrorCode::MAGIC_MISMATCH},
        {MakeHeader("TEST", 2, MM::MessageType::Call, 1, 1, 0, 0, 0),
         MM::ErrorCode::UNSUPPORTED_VERSION},
        {MakeHeader("TEST", 1, static_cast<MM::MessageType>(3), 1, 1, 0, 0, 0),
         MM::ErrorCode::INVALID_ARGUMENT},
        {MakeHeader("TEST", 1, MM::MessageType::Call, 1, 0x8001, 0, 0, 0),
         MM::ErrorCode::INVALID_ARGUMENT},
        {MakeHeader("TEST", 1, MM::MessageType::Notify, 1, 1, 0, 0, 0),
         MM::ErrorCode::INVALID_ARGUMENT},
        {MakeHeader("TEST", 1, MM::MessageType::Call, 1, 0, 0, 0, 0),
         MM::ErrorCode::INVALID_ARGUMENT},
        {MakeHeader("TEST", 1, MM::MessageType::Call, 1, 1, 0, 0,
                    static_cast<uint32_t>(MM::IPC_MAX_PAYLOAD) + 1),
         MM::ErrorCode::INVALID_ARGUMENT},
    };

    for (const Case& item : cases)
    {
        MM::detail::FrameParser parser({'T', 'E', 'S', 'T'}, 1,
                                       static_cast<uint32_t>(MM::IPC_MAX_PAYLOAD));

        uint8_t bytes[MM::IPC_HEADER_SIZE]{};
        MM::detail::EncodeHeader(item.hdr, bytes);
        parser.append(bytes, sizeof(bytes));

        MM::detail::Frame frame;
        const MM::detail::ParsedFrame parsed = parser.next(frame);
        CHECK(parsed == MM::detail::ParsedFrame::ERROR);
        CHECK(parser.error_code() == item.expected);
        CHECK(parser.has_header());
        CHECK(parser.last_header().cmd == item.hdr.cmd);
        CHECK(parser.last_header().seq_id == item.hdr.seq_id);
    }

    // header 不足 16 字节时不能报错，只能 NEED_MORE。
    MM::detail::FrameParser short_parser({'T', 'E', 'S', 'T'}, 1, 1024);
    const uint8_t partial[8] = {'T', 'E', 'S', 'T', 1, 0, 0x01, 0x00};
    short_parser.append(partial, sizeof(partial));

    MM::detail::Frame frame;
    CHECK(short_parser.next(frame) == MM::detail::ParsedFrame::NEED_MORE);
}

bool WriteAll(MM::CUnixSocksTransport& transport,
              const std::vector<uint8_t>& bytes)
{
    size_t sent = 0;
    while (sent < bytes.size())
    {
        const int n = transport.write(bytes.data() + sent,
                                      bytes.size() - sent);
        if (n > 0)
        {
            sent += static_cast<size_t>(n);
            continue;
        }
        if (n != -EAGAIN)
            return false;
        transport.wait(true, 5);
    }
    return true;
}

bool ReadFrame(MM::CUnixSocksTransport& transport,
               MM::detail::FrameParser& parser,
               MM::detail::Frame& out, int timeout_ms)
{
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeout_ms);

    while (std::chrono::steady_clock::now() < deadline)
    {
        uint8_t buffer[512];
        const int n = transport.read(buffer, sizeof(buffer));

        if (n > 0)
        {
            parser.append(buffer, static_cast<size_t>(n));
            const MM::detail::ParsedFrame parsed = parser.next(out);
            if (parsed == MM::detail::ParsedFrame::OK)
                return true;
            if (parsed == MM::detail::ParsedFrame::ERROR)
                return false;
            continue;
        }

        if (n == 0)
            return false;
        if (n != -EAGAIN)
            return false;

        transport.wait(false, 5);
    }

    return false;
}

void CheckFramingErrorReply(const MM::detail::IpcHeader& bad,
                            MM::ErrorCode expected)
{
    TransportPair pair;
    if (!pair.connect())
    {
        CHECK(false);
        return;
    }

    MM::MessageManager server(MakeConfig(MM::MMMode::Server));
    CHECK(server.assign_transport(pair.server) == 0);
    CHECK(server.start() == 0);

    const std::vector<uint8_t> bytes =
        MM::detail::EncodeFrame(bad, MM::Payload{});
    CHECK(WriteAll(pair.client, bytes));

    MM::detail::FrameParser parser({'T', 'E', 'S', 'T'}, 1,
                                   static_cast<uint32_t>(MM::IPC_MAX_PAYLOAD));
    MM::detail::Frame reply;
    CHECK(ReadFrame(pair.client, parser, reply, 2000));

    CHECK(reply.hdr.type == static_cast<uint8_t>(MM::MessageType::Response));
    CHECK(reply.hdr.cmd == MM::IPC_CMD_GENERAL_ERROR);
    CHECK(MM::IsErrorFlag(reply.hdr.flag));
    CHECK(reply.hdr.code == static_cast<uint8_t>(expected));

    server.end();
}

void TestFramingErrorReplyCommand()
{
    // magic / ver / 保留 cmd 都由 framing 层回 cmd=0x0000。
    CheckFramingErrorReply(
        MakeHeader("BAD!", 1, MM::MessageType::Call, 0x1234, 1, 0, 0, 0),
        MM::ErrorCode::MAGIC_MISMATCH);

    CheckFramingErrorReply(
        MakeHeader("TEST", 2, MM::MessageType::Call, 0x1234, 1, 0, 0, 0),
        MM::ErrorCode::UNSUPPORTED_VERSION);

    CheckFramingErrorReply(
        MakeHeader("TEST", 1, MM::MessageType::Call,
                   MM::IPC_CMD_GENERAL_ERROR, 1, 0, 0, 0),
        MM::ErrorCode::INVALID_ARGUMENT);
}

// ---------------------------------------------------------------------------
// end-to-end
// ---------------------------------------------------------------------------

void TestRequestResponse()
{
    TransportPair pair;
    if (!pair.connect())
    {
        CHECK(false);
        return;
    }

    MM::MessageManager server(MakeConfig(MM::MMMode::Server));
    MM::MessageManager client(MakeConfig(MM::MMMode::Client));
    CHECK(server.assign_transport(pair.server) == 0);
    CHECK(client.assign_transport(pair.client) == 0);
    CHECK(server.start() == 0);
    CHECK(client.start() == 0);

    const MM::Payload request = {0x00, 0xFF, 0x10};
    const int id = client.send(0x0301, request, MM::SendMode::Request);
    CHECK(id > 0);

    MM::Message incoming;
    CHECK(FetchRequestWait(server, &incoming, 2000) == id);
    CHECK(incoming.type == MM::MessageType::Call);
    CHECK(incoming.cmd == 0x0301);
    CHECK(incoming.seq_id == static_cast<uint16_t>(id));
    CHECK(incoming.payload == request);

    const MM::Payload response = {'o', 'k'};
    CHECK(server.reply(incoming.id, incoming.cmd, response) == 0);

    MM::Message outgoing;
    CHECK(RecvWait(client, id, &outgoing, 2000) == id);
    CHECK(outgoing.type == MM::MessageType::Response);
    CHECK(outgoing.cmd == 0x0301);
    CHECK(outgoing.flag == 0);
    CHECK(outgoing.code == 0);
    CHECK(outgoing.payload == response);

    CHECK(client.recv(id, &outgoing) ==
          static_cast<int>(MM::Status::NOT_PENDING));

    client.end();
    server.end();
}

void TestReplyError()
{
    TransportPair pair;
    if (!pair.connect())
    {
        CHECK(false);
        return;
    }

    MM::MessageManager server(MakeConfig(MM::MMMode::Server));
    MM::MessageManager client(MakeConfig(MM::MMMode::Client));
    CHECK(server.assign_transport(pair.server) == 0);
    CHECK(client.assign_transport(pair.client) == 0);
    CHECK(server.start() == 0);
    CHECK(client.start() == 0);

    const int id = client.send(0x0301, MM::Payload{});
    CHECK(id > 0);

    MM::Message request;
    CHECK(FetchRequestWait(server, &request, 2000) == id);
    CHECK(server.reply_error(request.id, request.cmd,
                             static_cast<uint8_t>(MM::ErrorCode::NOT_SUPPORTED),
                             "unknown command") == 0);

    MM::Message response;
    CHECK(RecvWait(client, id, &response, 2000) == id);
    CHECK(MM::IsErrorFlag(response.flag));
    CHECK(response.code ==
          static_cast<uint8_t>(MM::ErrorCode::NOT_SUPPORTED));
    CHECK(std::string(response.payload.begin(), response.payload.end()) ==
          "unknown command");

    client.end();
    server.end();
}

void TestOneWay()
{
    TransportPair pair;
    if (!pair.connect())
    {
        CHECK(false);
        return;
    }

    MM::MessageManager server(MakeConfig(MM::MMMode::Server));
    MM::MessageManager client(MakeConfig(MM::MMMode::Client));
    CHECK(server.assign_transport(pair.server) == 0);
    CHECK(client.assign_transport(pair.client) == 0);
    CHECK(server.start() == 0);
    CHECK(client.start() == 0);

    const int id = client.send(0x0201, MM::Payload{1, 2},
                               MM::SendMode::OneWay);
    CHECK(id > 0);

    MM::Message request;
    CHECK(FetchRequestWait(server, &request, 2000) == id);
    CHECK((request.flag & MM::IPC_FLAG_NO_RESPONSE) != 0);

    MM::Message response;
    CHECK(client.recv(id, &response) ==
          static_cast<int>(MM::Status::NOT_PENDING));
    CHECK(client.discard(id) == static_cast<int>(MM::Status::NOT_FOUND));

    // OneWay 不进入 pending；timeout 只对 Request 生效。
    const int oneway = client.send(0x0202, MM::Payload{},
                                   MM::SendMode::OneWay, 100);
    CHECK(oneway > 0);
    CHECK(client.recv(oneway, &response) ==
          static_cast<int>(MM::Status::NOT_PENDING));
    CHECK(client.discard(oneway) == static_cast<int>(MM::Status::NOT_FOUND));

    client.end();
    server.end();
}

void TestNotify()
{
    TransportPair pair;
    if (!pair.connect())
    {
        CHECK(false);
        return;
    }

    MM::MessageManager server(MakeConfig(MM::MMMode::Server));
    MM::MessageManager client(MakeConfig(MM::MMMode::Client));
    CHECK(server.assign_transport(pair.server) == 0);
    CHECK(client.assign_transport(pair.client) == 0);
    CHECK(server.start() == 0);
    CHECK(client.start() == 0);

    const MM::Payload event_payload = {'e', 'v'};
    CHECK(server.notify(0x0401, event_payload) == 0);

    MM::Message event;
    CHECK(FetchNotifyWait(client, &event, 2000) > 0);
    CHECK(event.type == MM::MessageType::Notify);
    CHECK((event.seq_id & MM::IPC_SEQ_NOTIFY_MASK) != 0);
    CHECK(event.cmd == 0x0401);
    CHECK(event.payload == event_payload);

    CHECK(client.fetch_notify(&event) == 0);

    client.end();
    server.end();
}

void TestCreateSocks()
{
    const std::string path = MM::MakeTempUnixSocketPath("mm-test");
    if (path.empty())
    {
        CHECK(false);
        return;
    }

    MM::CUnixSocksTransport server;
    MM::CUnixSocksTransport client;
    std::atomic<int> server_rc{-999};

    std::thread acceptor([&server, &path, &server_rc] {
        server_rc.store(server.CreateSocks(path));
    });

    int client_rc = -1;
    for (int i = 0; i < 200 && client_rc < 0; ++i)
    {
        client_rc = client.ConnectSocks(path);
        if (client_rc < 0)
            SleepMs(10);
    }

    acceptor.join();

    CHECK(server_rc.load() == 0);
    CHECK(client_rc == 0);
    CHECK(server.IsOpen());
    CHECK(client.IsOpen());

    server.close();
    client.close();
}

void TestTimeoutAndDiscard()
{
    TransportPair pair;
    if (!pair.connect())
    {
        CHECK(false);
        return;
    }

    MM::MessageManager server(MakeConfig(MM::MMMode::Server));
    MM::MessageManager client(MakeConfig(MM::MMMode::Client));
    CHECK(server.assign_transport(pair.server) == 0);
    CHECK(client.assign_transport(pair.client) == 0);
    CHECK(server.start() == 0);
    CHECK(client.start() == 0);

    // deadline 由 send 提供。
    const int id = client.send(0x0302, MM::Payload{}, MM::SendMode::Request, 30);
    CHECK(id > 0);

    MM::Message response;
    CHECK(client.recv(id, &response) == static_cast<int>(MM::Status::AGAIN));

    SleepMs(60);
    CHECK(client.recv(id, &response) == 0);
    CHECK(client.recv(id, &response) ==
          static_cast<int>(MM::Status::NOT_PENDING));

    // 主动放弃：pending 立即释放。
    const int other = client.send(0x0304, MM::Payload{});
    CHECK(other > 0);
    CHECK(client.recv(other, &response) == static_cast<int>(MM::Status::AGAIN));
    CHECK(client.discard(other) == 0);
    CHECK(client.recv(other, &response) ==
          static_cast<int>(MM::Status::NOT_PENDING));
    CHECK(client.discard(other) == static_cast<int>(MM::Status::NOT_FOUND));

    client.end();
    server.end();
}

void TestDiscardAsync()
{
    TransportPair pair;
    if (!pair.connect())
    {
        CHECK(false);
        return;
    }

    MM::MessageManager server(MakeConfig(MM::MMMode::Server));
    MM::MessageManager client(MakeConfig(MM::MMMode::Client));
    CHECK(server.assign_transport(pair.server) == 0);
    CHECK(client.assign_transport(pair.client) == 0);
    CHECK(server.start() == 0);
    CHECK(client.start() == 0);

    const int id = client.send(0x0303, MM::Payload{});
    CHECK(id > 0);

    MM::Message response;
    CHECK(client.recv(id, &response) == static_cast<int>(MM::Status::AGAIN));
    CHECK(client.discard(id) == 0);
    CHECK(client.recv(id, &response) ==
          static_cast<int>(MM::Status::NOT_PENDING));
    CHECK(client.discard(id) == static_cast<int>(MM::Status::NOT_FOUND));

    client.end();
    server.end();
}

void TestExpiredButReady()
{
    TransportPair pair;
    if (!pair.connect())
    {
        CHECK(false);
        return;
    }

    MM::MessageManager server(MakeConfig(MM::MMMode::Server));
    MM::MessageManager client(MakeConfig(MM::MMMode::Client));
    CHECK(server.assign_transport(pair.server) == 0);
    CHECK(client.assign_transport(pair.client) == 0);
    CHECK(server.start() == 0);
    CHECK(client.start() == 0);

    const int id = client.send(0x0305, MM::Payload{},
                               MM::SendMode::Request, 100);
    CHECK(id > 0);

    MM::Message request;
    CHECK(FetchRequestWait(server, &request, 2000) == id);
    CHECK(server.reply(request.id, request.cmd, MM::Payload{}) == 0);

    // deadline 已过，但 response 在 deadline 前已经到了：ready 优先。
    SleepMs(150);

    MM::Message response;
    CHECK(client.recv(id, &response) == id);

    client.end();
    server.end();
}

void TestModeConstraints()
{
    MM::MessageManager server(MakeConfig(MM::MMMode::Server));
    MM::MessageManager client(MakeConfig(MM::MMMode::Client));
    MM::Message message;

    CHECK(server.send(1, MM::Payload{}, MM::SendMode::Request) ==
          static_cast<int>(MM::Status::NOT_SUPPORTED));
    CHECK(server.recv(1, &message) ==
          static_cast<int>(MM::Status::NOT_SUPPORTED));
    CHECK(server.fetch_notify(&message) ==
          static_cast<int>(MM::Status::NOT_SUPPORTED));
    CHECK(server.discard(1) ==
          static_cast<int>(MM::Status::NOT_SUPPORTED));

    CHECK(client.fetch_request(&message) ==
          static_cast<int>(MM::Status::NOT_SUPPORTED));
    CHECK(client.reply(1, 1, MM::Payload{}) ==
          static_cast<int>(MM::Status::NOT_SUPPORTED));
    CHECK(client.reply_error(1, 1, 1, "x") ==
          static_cast<int>(MM::Status::NOT_SUPPORTED));
    CHECK(client.notify(1, MM::Payload{}) ==
          static_cast<int>(MM::Status::NOT_SUPPORTED));

    // 没有 transport 时不能 start。
    CHECK(server.start() < 0);
    CHECK(client.start() < 0);
}

void TestMagicMismatch()
{
    TransportPair pair;
    if (!pair.connect())
    {
        CHECK(false);
        return;
    }

    MM::MessageManager server(MakeConfig(MM::MMMode::Server, "SRV1"));
    MM::MessageManager client(MakeConfig(MM::MMMode::Client, "CLI1"));

    std::atomic<bool> server_down{false};
    std::atomic<bool> client_down{false};
    std::atomic<int> server_reason{0};
    std::atomic<int> client_reason{0};

    server.set_disconnect_callback([&server_down, &server_reason](int reason) {
        server_reason.store(reason);
        server_down.store(true);
    });
    client.set_disconnect_callback([&client_down, &client_reason](int reason) {
        client_reason.store(reason);
        client_down.store(true);
    });

    CHECK(server.assign_transport(pair.server) == 0);
    CHECK(client.assign_transport(pair.client) == 0);
    CHECK(server.start() == 0);
    CHECK(client.start() == 0);

    const int id = client.send(0x0301, MM::Payload{});
    CHECK(id > 0);

    CHECK(WaitFor(server_down, 2000));
    CHECK(WaitFor(client_down, 2000));
    CHECK(server_reason.load() ==
          static_cast<int>(MM::DisconnectReason::PROTOCOL_ERROR));

    MM::Message response;
    CHECK(RecvWait(client, id, &response, 2000) ==
          static_cast<int>(MM::Status::CLOSED));

    client.end();
    server.end();
}

void TestVersionMismatch()
{
    TransportPair pair;
    if (!pair.connect())
    {
        CHECK(false);
        return;
    }

    MM::MessageManager server(MakeConfig(MM::MMMode::Server, "TEST", 1));
    MM::MessageManager client(MakeConfig(MM::MMMode::Client, "TEST", 2));

    std::atomic<bool> server_down{false};
    std::atomic<bool> client_down{false};

    server.set_disconnect_callback([&server_down](int) {
        server_down.store(true);
    });
    client.set_disconnect_callback([&client_down](int) {
        client_down.store(true);
    });

    CHECK(server.assign_transport(pair.server) == 0);
    CHECK(client.assign_transport(pair.client) == 0);
    CHECK(server.start() == 0);
    CHECK(client.start() == 0);

    CHECK(client.send(0x0301, MM::Payload{}) > 0);
    CHECK(WaitFor(server_down, 2000));
    CHECK(WaitFor(client_down, 2000));

    client.end();
    server.end();
}

void TestRequestQueueFull()
{
    TransportPair pair;
    if (!pair.connect())
    {
        CHECK(false);
        return;
    }

    MM::MessageManagerConfig server_config = MakeConfig(MM::MMMode::Server);
    server_config.max_request_queue = 1;

    MM::MessageManager server(server_config);
    MM::MessageManager client(MakeConfig(MM::MMMode::Client));
    CHECK(server.assign_transport(pair.server) == 0);
    CHECK(client.assign_transport(pair.client) == 0);
    CHECK(server.start() == 0);
    CHECK(client.start() == 0);

    const int first = client.send(0x0301, MM::Payload{});
    const int second = client.send(0x0302, MM::Payload{});
    CHECK(first > 0);
    CHECK(second > 0);

    MM::Message response;
    CHECK(RecvWait(client, second, &response, 2000) == second);
    CHECK(MM::IsErrorFlag(response.flag));
    CHECK(response.code == static_cast<uint8_t>(MM::ErrorCode::BUSY));
    CHECK(std::string(response.payload.begin(), response.payload.end()) ==
          "queue full");

    MM::Message request;
    CHECK(FetchRequestWait(server, &request, 2000) == first);
    CHECK(request.cmd == 0x0301);

    client.end();
    server.end();
}

void TestPeerClosed()
{
    TransportPair pair;
    if (!pair.connect())
    {
        CHECK(false);
        return;
    }

    MM::MessageManager server(MakeConfig(MM::MMMode::Server));
    MM::MessageManager client(MakeConfig(MM::MMMode::Client));

    std::atomic<bool> down{false};
    std::atomic<int> reason{0};
    client.set_disconnect_callback([&down, &reason](int value) {
        reason.store(value);
        down.store(true);
    });

    CHECK(server.assign_transport(pair.server) == 0);
    CHECK(client.assign_transport(pair.client) == 0);
    CHECK(server.start() == 0);
    CHECK(client.start() == 0);

    const int id = client.send(0x0301, MM::Payload{});
    CHECK(id > 0);

    // 先取走 request，避免 close 时还有未读数据导致 RST。
    MM::Message request;
    CHECK(FetchRequestWait(server, &request, 2000) == id);

    server.end();

    CHECK(WaitFor(down, 2000));
    CHECK(reason.load() ==
          static_cast<int>(MM::DisconnectReason::PEER_CLOSED));

    MM::Message response;
    CHECK(RecvWait(client, id, &response, 2000) ==
          static_cast<int>(MM::Status::CLOSED));

    client.end();
}

void TestStressRequestResponse()
{
    TransportPair pair;
    if (!pair.connect())
    {
        CHECK(false);
        return;
    }

    MM::MessageManager server(MakeStressConfig(MM::MMMode::Server));
    MM::MessageManager client(MakeStressConfig(MM::MMMode::Client));
    CHECK(server.assign_transport(pair.server) == 0);
    CHECK(client.assign_transport(pair.client) == 0);
    CHECK(server.start() == 0);
    CHECK(client.start() == 0);

    constexpr int kProducers = 4;
    constexpr int kPerProducer = 500;
    constexpr int kTotal = kProducers * kPerProducer;
    constexpr int kServerWorkers = 2;

    std::atomic<int> handled{0};
    std::atomic<int> failures{0};
    std::atomic<bool> stop{false};

    std::vector<std::thread> workers;
    for (int w = 0; w < kServerWorkers; ++w)
    {
        workers.emplace_back([&] {
            while (!stop.load())
            {
                MM::Message req;
                const int id = server.fetch_request(&req);

                if (id > 0)
                {
                    if (server.reply(req.id, req.cmd, req.payload) == 0)
                        handled.fetch_add(1);
                    else
                        failures.fetch_add(1);
                }
                else if (id < 0)
                {
                    break;
                }
                else
                {
                    std::this_thread::yield();
                }
            }
        });
    }

    const auto start = std::chrono::steady_clock::now();
    std::vector<std::thread> producers;

    for (int p = 0; p < kProducers; ++p)
    {
        producers.emplace_back([&, p] {
            std::vector<int> ids;
            std::vector<MM::Payload> payloads;
            ids.reserve(kPerProducer);
            payloads.reserve(kPerProducer);

            // 先全部发出去，制造大量 in-flight request。
            for (int i = 0; i < kPerProducer; ++i)
            {
                const uint32_t value =
                    static_cast<uint32_t>(p * kPerProducer + i);
                MM::Payload payload = MakeValuePayload(value);

                int id = -1;
                for (int retry = 0; retry < 10000; ++retry)
                {
                    id = client.send(0x0301, payload, MM::SendMode::Request,
                                     10000);
                    if (id > 0)
                        break;
                    if (id != static_cast<int>(MM::Status::BUSY))
                        break;
                    std::this_thread::yield();
                }

                if (id <= 0)
                {
                    failures.fetch_add(1);
                    continue;
                }

                ids.push_back(id);
                payloads.push_back(std::move(payload));
            }

            const auto recv_deadline = std::chrono::steady_clock::now() +
                                       std::chrono::seconds(10);
            for (size_t i = 0; i < ids.size(); ++i)
            {
                const auto now = std::chrono::steady_clock::now();
                int remaining_ms = 0;
                if (now < recv_deadline)
                {
                    remaining_ms = static_cast<int>(
                        std::chrono::duration_cast<std::chrono::milliseconds>(
                            recv_deadline - now)
                            .count());
                }

                MM::Message response;
                const int rc = RecvWait(client, ids[i], &response, remaining_ms);
                if (rc != ids[i] || response.payload != payloads[i])
                    failures.fetch_add(1);
            }
        });
    }

    for (auto& t : producers)
        t.join();

    const double elapsed = std::chrono::duration<double>(
                               std::chrono::steady_clock::now() - start)
                               .count();

    stop.store(true);
    for (auto& t : workers)
        t.join();

    CHECK(failures.load() == 0);
    CHECK(handled.load() == kTotal);

    const double rate = elapsed > 0.0 ? kTotal / elapsed : 0.0;
    std::printf("stress rpc:    %d messages in %.3f s (%.0f msg/s)\n",
                kTotal, elapsed, rate);

    client.end();
    server.end();
}

void TestStressNotify()
{
    TransportPair pair;
    if (!pair.connect())
    {
        CHECK(false);
        return;
    }

    MM::MessageManager server(MakeStressConfig(MM::MMMode::Server));
    MM::MessageManager client(MakeStressConfig(MM::MMMode::Client));
    CHECK(server.assign_transport(pair.server) == 0);
    CHECK(client.assign_transport(pair.client) == 0);
    CHECK(server.start() == 0);
    CHECK(client.start() == 0);

    constexpr int kTotal = 4000;
    std::atomic<int> failures{0};
    std::atomic<bool> producer_done{false};

    const auto start = std::chrono::steady_clock::now();
    std::thread producer([&] {
        for (int i = 0; i < kTotal; ++i)
        {
            const MM::Payload payload =
                MakeValuePayload(static_cast<uint32_t>(i));

            int rc = -1;
            for (int retry = 0; retry < 10000; ++retry)
            {
                rc = server.notify(0x0401, payload);
                if (rc == 0)
                    break;
                if (rc != static_cast<int>(MM::Status::BUSY))
                    break;
                std::this_thread::yield();
            }

            if (rc != 0)
            {
                failures.fetch_add(1);
                break;
            }
        }
        producer_done.store(true);
    });

    std::vector<uint8_t> seen(kTotal, 0);
    int received = 0;
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(10);

    while (received < kTotal &&
           std::chrono::steady_clock::now() < deadline)
    {
        MM::Message event;
        const int rc = client.fetch_notify(&event);

        if (rc > 0)
        {
            const uint32_t value = PayloadValue(event.payload);
            if (value < static_cast<uint32_t>(kTotal) && !seen[value])
            {
                seen[value] = 1;
                ++received;
            }
            else
            {
                failures.fetch_add(1);
            }
        }
        else if (rc < 0)
        {
            break;
        }
        else
        {
            std::this_thread::yield();
        }
    }

    producer.join();

    const double elapsed = std::chrono::duration<double>(
                               std::chrono::steady_clock::now() - start)
                               .count();

    CHECK(failures.load() == 0);
    CHECK(received == kTotal);
    CHECK(producer_done.load());

    const double rate = elapsed > 0.0 ? kTotal / elapsed : 0.0;
    std::printf("stress notify: %d messages in %.3f s (%.0f msg/s)\n",
                kTotal, elapsed, rate);

    client.end();
    server.end();
}

void TestStressOneWay()
{
    TransportPair pair;
    if (!pair.connect())
    {
        CHECK(false);
        return;
    }

    MM::MessageManager server(MakeStressConfig(MM::MMMode::Server));
    MM::MessageManager client(MakeStressConfig(MM::MMMode::Client));
    CHECK(server.assign_transport(pair.server) == 0);
    CHECK(client.assign_transport(pair.client) == 0);
    CHECK(server.start() == 0);
    CHECK(client.start() == 0);

    constexpr int kTotal = 4000;
    std::atomic<int> failures{0};

    const auto start = std::chrono::steady_clock::now();
    std::thread producer([&] {
        for (int i = 0; i < kTotal; ++i)
        {
            const MM::Payload payload =
                MakeValuePayload(static_cast<uint32_t>(i));

            int id = -1;
            for (int retry = 0; retry < 10000; ++retry)
            {
                id = client.send(0x0201, payload, MM::SendMode::OneWay);
                if (id > 0)
                    break;
                if (id != static_cast<int>(MM::Status::BUSY))
                    break;
                std::this_thread::yield();
            }

            if (id <= 0)
            {
                failures.fetch_add(1);
                break;
            }
        }
    });

    std::vector<uint8_t> seen(kTotal, 0);
    int received = 0;
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(10);

    while (received < kTotal &&
           std::chrono::steady_clock::now() < deadline)
    {
        MM::Message request;
        const int id = server.fetch_request(&request);

        if (id > 0)
        {
            const uint32_t value = PayloadValue(request.payload);
            if (value < static_cast<uint32_t>(kTotal) && !seen[value])
            {
                seen[value] = 1;
                ++received;
            }
            else
            {
                failures.fetch_add(1);
            }
        }
        else if (id < 0)
        {
            break;
        }
        else
        {
            std::this_thread::yield();
        }
    }

    producer.join();

    const double elapsed = std::chrono::duration<double>(
                               std::chrono::steady_clock::now() - start)
                               .count();

    CHECK(failures.load() == 0);
    CHECK(received == kTotal);

    const double rate = elapsed > 0.0 ? kTotal / elapsed : 0.0;
    std::printf("stress oneway: %d messages in %.3f s (%.0f msg/s)\n",
                kTotal, elapsed, rate);

    client.end();
    server.end();
}

} // namespace

int main()
{
    TestDefaultConfig();
    TestFramingEncoding();
    TestFramingIncremental();
    TestFramingErrors();
    TestFramingErrorReplyCommand();
    TestRequestResponse();
    TestReplyError();
    TestOneWay();
    TestNotify();
    TestCreateSocks();
    TestTimeoutAndDiscard();
    TestDiscardAsync();
    TestExpiredButReady();
    TestModeConstraints();
    TestMagicMismatch();
    TestVersionMismatch();
    TestRequestQueueFull();
    TestPeerClosed();
    TestStressRequestResponse();
    TestStressNotify();
    TestStressOneWay();

    if (g_failures.load() == 0)
    {
        std::printf("message_manager_test: all checks passed\n");
        return 0;
    }

    std::printf("message_manager_test: %d check(s) failed\n",
                g_failures.load());
    return 1;
}
