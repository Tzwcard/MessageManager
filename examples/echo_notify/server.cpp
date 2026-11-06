// echo / notify sample server.
//
// 启动后阻塞等待一个 client；之后：
//   - client 发 ECHO，server 原样回 payload；
//   - 在 server 的终端里输入一行并回车，server 发 NOTIFY 给 client。

#include "sample_common.h"

#include <MessageManager.h>
#include <UnixSocksTransport.h>

#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

int main(int argc, char** argv)
{
    const std::string socket_path =
        Sample::SocketPath(argc > 0 ? argv[0] : nullptr);

    std::printf("server: socket %s\n", socket_path.c_str());
    std::printf("server: waiting for client...\n");

    MessageManager::CUnixSocksTransport transport;
    const int listen_rc = transport.CreateSocks(socket_path);
    if (listen_rc != 0)
    {
        std::fprintf(stderr, "server: listen failed: %d\n", listen_rc);
        return 1;
    }

    MessageManager::MessageManager server(
        Sample::MakeConfig(MessageManager::MMMode::Server));

    if (server.assign_transport(transport) != 0 || server.start() != 0)
    {
        std::fprintf(stderr, "server: MessageManager start failed\n");
        return 1;
    }

    std::printf("server: client connected\n");
    std::printf("server: type text + Enter to send NOTIFY, quit to exit\n");

    for (;;)
    {
        // 先处理所有已到达的 ECHO。
        MessageManager::Message request;
        int id = 0;
        while ((id = server.fetch_request(&request)) > 0)
        {
            if (request.cmd == Sample::kCmdEcho)
            {
                const std::string text = Sample::PayloadText(request.payload);
                server.reply(request.id, request.cmd, request.payload);
                std::printf("server: ECHO \"%s\"\n", text.c_str());
            }
            else
            {
                server.reply_error(
                    request.id, request.cmd,
                    static_cast<uint8_t>(
                        MessageManager::ErrorCode::NOT_SUPPORTED),
                    "unknown command");
            }
        }

        if (id < 0)
        {
            std::printf("server: client disconnected\n");
            break;
        }

        // 最多等 50 ms 输入，让 fetch_request 有机会及时处理。
        std::string line;
        const Sample::StdinState state = Sample::ReadLine(50, line);
        if (state == Sample::StdinState::Eof)
            break;
        if (state == Sample::StdinState::Timeout)
            continue;

        if (line == "quit")
            break;

        if (server.notify(Sample::kCmdNotify, Sample::TextPayload(line)) != 0)
        {
            std::fprintf(stderr, "server: notify failed\n");
            break;
        }

        std::printf("server: NOTIFY \"%s\"\n", line.c_str());
    }

    server.end();
    std::printf("server: bye\n");
    return 0;
}
