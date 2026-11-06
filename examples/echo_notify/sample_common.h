#pragma once

#include <MessageDefs.h>
#include <MessageManager.h>

#include <array>
#include <cerrno>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <poll.h>
#include <string>
#include <unistd.h>

namespace Sample
{
// service 名，同时作为 framing magic（必须正好 4 字节）。
constexpr std::array<uint8_t, 4> kMagic = {
    static_cast<uint8_t>('1'),
    static_cast<uint8_t>('2'),
    static_cast<uint8_t>('3'),
    static_cast<uint8_t>('4'),
};

constexpr uint16_t kCmdEcho = 0x0001; // client -> server -> client
constexpr uint16_t kCmdNotify = 0x0002; // server -> client，单向

inline MessageManager::Payload TextPayload(const std::string& text)
{
    return MessageManager::Payload(text.begin(), text.end());
}

inline std::string PayloadText(const MessageManager::Payload& payload)
{
    return std::string(payload.begin(), payload.end());
}

inline MessageManager::MessageManagerConfig MakeConfig(
    MessageManager::MMMode mode)
{
    MessageManager::MessageManagerConfig config;
    config.magic = kMagic;
    config.version = 1;
    config.mode = mode;
    return config;
}

inline std::string ExecutableDirectory(const char* argv0)
{
#if defined(__linux__)
    {
        std::error_code ec;
        const std::filesystem::path self =
            std::filesystem::read_symlink("/proc/self/exe", ec);
        if (!ec)
            return self.parent_path().string();
    }
#endif

    std::error_code ec;
    const std::filesystem::path exe =
        std::filesystem::absolute(argv0 != nullptr ? argv0 : ".", ec);
    if (ec)
        return std::string(".");
    return exe.parent_path().string();
}

// client / server 都用可执行文件同目录下的 sample.socks。
inline std::string SocketPath(const char* argv0)
{
    return (std::filesystem::path(ExecutableDirectory(argv0)) / "sample.socks")
        .string();
}

enum class StdinState
{
    Line,
    Timeout,
    Eof,
};

// 最多等 timeout_ms。返回 Line 时 line 是去掉换行的一行输入。
inline StdinState ReadLine(int timeout_ms, std::string& line)
{
    struct pollfd pfd;
    pfd.fd = STDIN_FILENO;
    pfd.events = POLLIN;
    pfd.revents = 0;

    for (;;)
    {
        const int rc = ::poll(&pfd, 1, timeout_ms);
        if (rc == 0)
            return StdinState::Timeout;
        if (rc < 0)
        {
            if (errno == EINTR)
                continue;
            return StdinState::Eof;
        }
        break;
    }

    if (!std::getline(std::cin, line))
        return StdinState::Eof;

    return StdinState::Line;
}
} // namespace Sample
