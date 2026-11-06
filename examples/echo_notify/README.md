# ECHO / NOTIFY sample

最小的 server ↔ client 交互示例：

| command | 方向 | 行为 |
|---|---|---|
| `ECHO = 0x0001` | client → server → client | request 的 payload 原样作为 response 的 payload |
| `NOTIFY = 0x0002` | server → client | 单向，无 response |

```text
service name / magic = "1234"
version              = 1
socket               = <可执行文件所在目录>/sample.socks
```

payload 都是 UTF-8 纯文本。MM 只负责 framing / 队列 / 匹配，文本编解码在
`sample_common.h` 里。

## 构建

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --target mm_sample_server mm_sample_client -j
```

## 运行

先起 server（它会阻塞到 client 连上）：

```bash
./build/examples/echo_notify/mm_sample_server
```

另一个终端起 client：

```bash
./build/examples/echo_notify/mm_sample_client
```

然后在 **client** 终端输入文本并回车：

```text
client: type text + Enter to send ECHO, quit to exit
hello
client: ECHO "hello"
```

在 **server** 终端输入文本并回车，client 会异步打印：

```text
server: NOTIFY "world"
client: NOTIFY "world"
```

两边都输入 `quit` 或者 Ctrl+D 退出。
