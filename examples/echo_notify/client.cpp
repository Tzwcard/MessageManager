// echo / notify sample client.
//
// 输入一行并回车：发 ECHO，打印 server 回来的 response payload。
// server 发的 NOTIFY 会异步打印出来。

#include "sample_common.h"

#include <MessageManager.h>
#include <UnixSocksTransport.h>

#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

namespace
{
// 非阻塞消费所有 Notify。返回 false 表示连接已关闭。
bool DrainNotifications(MessageManager::MessageManager& client)
{
    MessageManager::Message note;
    for (;;)
    {
        const int rc = client.fetch_notify(&note);
        if (rc > 0)
        {
            std::printf("client: NOTIFY \"%s\"\n",
                        Sample::PayloadText(note.payload).c_str());
            continue;
        }
        if (rc == 0)
            return true;
        return false;
    }
}

bool WaitResponse(MessageManager::MessageManager& client, int id,
                  MessageManager::Message& response)
{
    for (;;)
    {
        const int rc = client.recv(
            static_cast<MessageManager::MessageId>(id), &response);
        if (rc > 0)
            return true;
        if (rc == 0)
        {
            std::fprintf(stderr, "client: request timed out\n");
            return false;
        }
        if (rc != static_cast<int>(MessageManager::Status::AGAIN))
        {
            std::fprintf(stderr, "client: recv failed: %d\n", rc);
            return false;
        }

        // 等 response 期间也把 Notify 打出来。
        if (!DrainNotifications(client))
            return false;

        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}
} // namespace

int main(int argc, char** argv)
{
    const std::string socket_path =
        Sample::SocketPath(argc > 0 ? argv[0] : nullptr);

    std::printf("client: socket %s\n", socket_path.c_str());

    MessageManager::CUnixSocksTransport transport;
    const int connect_rc = transport.ConnectSocks(socket_path);
    if (connect_rc != 0)
    {
        std::fprintf(stderr, "client: connect failed: %d\n", connect_rc);
        return 1;
    }

    MessageManager::MessageManager client(
        Sample::MakeConfig(MessageManager::MMMode::Client));

    if (client.assign_transport(transport) != 0 || client.start() != 0)
    {
        std::fprintf(stderr, "client: MessageManager start failed\n");
        return 1;
    }

    std::printf("client: connected\n");
    std::printf("client: type text + Enter to send ECHO, quit to exit\n");

    for (;;)
    {
        if (!DrainNotifications(client))
        {
            std::printf("client: connection closed\n");
            break;
        }

        std::string line;
        const Sample::StdinState state = Sample::ReadLine(50, line);
        if (state == Sample::StdinState::Eof)
            break;
        if (state == Sample::StdinState::Timeout)
            continue;

        if (line == "quit")
            break;

        const int id =
            client.send(Sample::kCmdEcho, Sample::TextPayload(line),
                        MessageManager::SendMode::Request, 3000);
        if (id <= 0)
        {
            std::fprintf(stderr, "client: send failed: %d\n", id);
            break;
        }

        MessageManager::Message response;
        if (!WaitResponse(client, id, response))
            break;

        if (MessageManager::IsErrorFlag(response.flag))
        {
            std::fprintf(stderr, "client: error %u: %s\n",
                         static_cast<unsigned>(response.code),
                         Sample::PayloadText(response.payload).c_str());
        }
        else
        {
            std::printf("client: ECHO \"%s\"\n",
                        Sample::PayloadText(response.payload).c_str());
        }
    }

    client.end();
    std::printf("client: bye\n");
    return 0;
}
