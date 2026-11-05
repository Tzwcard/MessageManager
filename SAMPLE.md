# SAMPLE: Unix Socks + MONITOR_*

```text
{MONITOR_SET_MODE, "MODE=MIRROR"} -> "OKAY"
{MONITOR_SET_MODE_STATE, nullptr} -> "WORKING" / "DONE"
{MONITOR_GET_MODE, nullptr}       -> "MIRROR"
```

> profile 示例，不是 MM 标准。payload 编解码属于 profile，MM 只传字节。

## Profile

```cpp
enum class Command : uint16_t
{
    MONITOR_SET_MODE       = 0x0101,
    MONITOR_SET_MODE_STATE = 0x0102,
    MONITOR_GET_MODE       = 0x0103,
};

namespace monitor
{
MM::Payload Serialize(const std::string& text)
{
    return MM::Payload(text.begin(), text.end());
}

std::string Unserialize(const MM::Payload& payload)
{
    return std::string(payload.begin(), payload.end());
}
} // namespace monitor
```

```text
magic = "MON!"
ver   = 1
```

## Server

```cpp
#include <UnixSocksTransport.h>

#include <chrono>
#include <thread>

MM::CUnixSocksTransport socket;
if (socket.CreateSocks("/run/msgmgr/monitor.sock") != 0) // 阻塞到 client 连上
    return 1;

MM::MessageManagerConfig config;
config.magic = {'M', 'O', 'N', '!'};
config.version = 1;
config.mode = MM::MMMode::Server;

MM::MessageManager server(config);
server.assign_transport(socket); // 非拥有，socket 要比 server 活得久
server.start();

std::string current_mode;
bool done = false;
int polls = 0;

while (running)
{
    MM::Message req;
    const int id = server.fetch_request(&req); // 非阻塞，0 = 队列空
    if (id < 0)
        break;
    if (id == 0)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        continue;
    }

    switch (static_cast<Command>(req.cmd))
    {
    case Command::MONITOR_SET_MODE:
    {
        const std::string body = monitor::Unserialize(req.payload);
        const bool ok = (body == "MODE=MIRROR");
        current_mode = ok ? "MIRROR" : "UNKNOWN";
        done = false;
        polls = 0;
        server.reply(req.id, req.cmd, monitor::Serialize(ok ? "OKAY" : "ERR"));
        break;
    }

    case Command::MONITOR_SET_MODE_STATE:
        if (++polls >= 2)
            done = true;
        server.reply(req.id, req.cmd,
                     monitor::Serialize(done ? "DONE" : "WORKING"));
        break;

    case Command::MONITOR_GET_MODE:
        server.reply(req.id, req.cmd,
                     monitor::Serialize(current_mode.empty()
                                            ? "UNKNOWN"
                                            : current_mode));
        break;

    default:
        server.reply_error(req.id, req.cmd,
                           static_cast<uint8_t>(MM::ErrorCode::NOT_SUPPORTED),
                           "unknown command");
        break;
    }
}

server.end();
```

## Client

```cpp
MM::CUnixSocksTransport socket;
if (socket.ConnectSocks("/run/msgmgr/monitor.sock") != 0)
    return 1;

MM::MessageManagerConfig config;
config.magic = {'M', 'O', 'N', '!'};
config.version = 1;
config.mode = MM::MMMode::Client;

MM::MessageManager client(config);
client.assign_transport(socket);
client.start();

auto request = [&client](Command cmd, const char* body = nullptr) -> std::string
{
    const MM::Payload payload =
        body ? monitor::Serialize(body) : MM::Payload{};

    const int id = client.send(static_cast<uint16_t>(cmd),
                               payload,
                               MM::SendMode::Request, 2000);
    if (id <= 0)
        return {};

    MM::Message resp;
    for (;;)
    {
        const int rc = client.recv(id, &resp); // 非阻塞
        if (rc > 0)
            break;
        if (rc == static_cast<int>(MM::Status::AGAIN))
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        return {}; // 0 = timeout, <0 = error
    }

    if (resp.flag & MM::IPC_FLAG_ERROR)
        return {};

    return monitor::Unserialize(resp.payload);
};

const std::string set_result = request(Command::MONITOR_SET_MODE, "MODE=MIRROR");

std::string state;
do
{
    state = request(Command::MONITOR_SET_MODE_STATE);
} while (state == "WORKING");

const std::string mode = request(Command::MONITOR_GET_MODE);
```

## Wire

```text
Call:      type=0  cmd=0x0101  seq=1  flag=0  code=0  payload="MODE=MIRROR"
Response:  type=1  cmd=0x0101  seq=1  flag=0  code=0  payload="OKAY"

Call:      type=0  cmd=0x0102  seq=2  flag=0  code=0  payload=""
Response:  type=1  cmd=0x0102  seq=2  flag=0  code=0  payload="WORKING"
Response:  type=1  cmd=0x0102  seq=3  flag=0  code=0  payload="DONE"

Call:      type=0  cmd=0x0103  seq=4  flag=0  code=0  payload=""
Response:  type=1  cmd=0x0103  seq=4  flag=0  code=0  payload="MIRROR"
```

## Link

Linux：`libMessageManager.a` + `libMessageManagerUnix.a`
Windows：`MessageManager.lib` + `MessageManagerUnix.lib`
