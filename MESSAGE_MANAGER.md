# MessageManager 设计

> MessageManager（MM）只负责 payload exchange：framing、队列、
> request/response 匹配、线程安全和连接生命周期。
>
> 它不解析 payload，不执行 command，不管理业务状态。
> **它也不定义、不特殊处理任何 `cmd`（包括 `SYS_*`）：所有 command 都由
> server / service 的 profile 定义和 dispatch。**

相关文档：

- [IPC_FRAMING.md](IPC_FRAMING.md)：wire framing
- [IPC_COMMANDS.md](IPC_COMMANDS.md)：command profile 样例 / 模板
- [transport/unix/README.md](transport/unix/README.md)：默认 AF_UNIX transport
- [../conn-service/IPC_COMMANDS.md](../conn-service/IPC_COMMANDS.md)：ConnService profile

---

## 1. 前提

- 1:1：一个 MessageManager 对应一个 transport；
- 多 client 暂不支持；以后再加 ClientRegistry / client_id。

### 生命周期：断开后必须重建（与 transport 无关）

一个 `MessageManager` 实例只服务**一条**连接。连接结束（对端 close /
I/O 错误 / framing 致命错误 / 自己调用 `end()`）后，实例进入终态：

```text
_closed = true      （永久）
_running = false
                    ↓
assign_transport()  -> INVALID
start()             -> CLOSED
send/reply/notify   -> CLOSED
fetch_* / recv      -> CLOSED
```

这是 MM 自身的状态机，**与具体 transport 实现无关**：

- `end()` 本身也是终态：即使连接还活着，调用 `end()` 之后实例也不能再
  `start()`；
- `end()` 只负责停止 reader、`shutdown()` + `close()` transport，
  **不会**把实例重置成可再次 `assign_transport()` 的状态；
- 不能通过“换一个 transport / 换一个 fd”让旧实例复活；
- 断开时 `pending`、`request_queue`、`notify_queue`、`outbound_queue`
  里的消息全部作废，未完成的 Request 需要上层决定是否重发。

重连的唯一合法方式：

```text
1. 停止并销毁旧 MessageManager
2. 上层准备好一条新连接对应的 transport
3. 构造新的 MessageManager，重新 assign_transport() + start()
```

伪代码（不关心 transport 是怎么来的）：

```cpp
for (;;)
{
    // 上层负责准备新连接：connect / accept / adopt / 自定义 transport
    ITransport& transport = ...;

    MessageManager mgr(config);
    mgr.assign_transport(transport);   // 非拥有，transport 必须活得比 mgr 久
    mgr.start();

    // ... 业务循环 ...

    mgr.end();
}   // 旧实例在这里析构，下一轮重新构造
```

transport 本身是否可以复用、如何重新 listen / accept，由具体 transport
自己的文档定义；MM 只要求“一个实例一条连接”。

---

## 2. 配置

```cpp
enum class MMMode
{
    Client,
    Server,
};

struct MessageManagerConfig
{
    std::array<uint8_t, 4> magic = {0x20, 0x26, 0x11, 0x05};
    uint8_t version = 1;            // IPC protocol + command set 版本
    MMMode mode = MMMode::Server;
    uint32_t max_payload = 1024 * 1024; // 1 MiB
    size_t max_request_queue = 64;
    size_t max_notify_queue = 64;
    size_t max_outbound_queue = 256;
};

explicit MessageManager(MessageManagerConfig config);
int assign_transport(std::unique_ptr<ITransport> transport); // 拥有
int assign_transport(ITransport& transport);                 // 非拥有
int assign_transport(ITransport* transport);                 // 非拥有
int start();
void end();
```

- `magic` / `version` 两端必须一致；
- `mode` 决定可用 API：
  - Client：`send` / `recv` / `fetch_notify` / `discard`；
  - Server：`fetch_request` / `reply` / `reply_error` / `notify`；
  - 模式不匹配返回 `NOT_SUPPORTED`；
- 非拥有形式要求 transport 比 MessageManager 活得久；
- framing 错误（magic / ver / type / seq / 长度）由 MM 直接回 error，
  `cmd = 0x0000 (GENERAL_ERROR)`；
- `0x0000` 是 framing 保留值。

---

## 3. Transport

MM 只依赖读写接口，不依赖 socket / poll / winsock：

```cpp
class ITransport
{
public:
    virtual ~ITransport() = default;

    // 非阻塞读：>0 字节数，0 = EOF，-EAGAIN = 暂无数据，<0 = 错误
    virtual int read(void* buf, size_t len) = 0;

    // 非阻塞写：>0 已写字节数，-EAGAIN = 暂时不可写，<0 = 错误
    virtual int write(const void* buf, size_t len) = 0;

    // 等待可读/可写；timeout_ms < 0 表示一直等
    // 返回 bitmask: 1=readable, 2=writable；0=timeout；<0=错误
    virtual int wait(bool want_write, int timeout_ms) = 0;

    // 从其他线程唤醒 wait()
    virtual void wake() = 0;

    // 让阻塞中的 read/write/wait 退出
    virtual void shutdown() = 0;

    virtual void close() = 0;
};
```

- 使用方可以自行实现 `ITransport`（socket、TLS、共享内存、测试 channel）；
- 默认 fallback 是 `transport/unix` 的 `CUnixSocksTransport`
  （AF_UNIX SOCK_STREAM，自己管理 path / listen / connect / accept）；
- 不链接 `MessageManagerUnix` 时，core 完全不包含平台 I/O 代码；
- reader loop 只调用 `read/write/wait/wake/shutdown`。

---

## 4. 数据结构

```cpp
using MessageId = uint64_t;

enum class MessageType : uint8_t
{
    Call = 0,
    Response = 1,
    Notify = 2,
};

enum class SendMode
{
    Request,   // 需要 response，进入 pending map
    OneWay,    // 不需要 response，不进入 pending map
};

struct Message
{
    MessageId id = 0;       // MM 本地 id（当前实现等于 seq_id）
    MessageType type = MessageType::Call;
    uint16_t cmd = 0;
    uint8_t flag = 0;       // IPC_FLAG_*
    uint8_t code = 0;       // flag & IPC_FLAG_ERROR 时为 error code
    uint16_t seq_id = 0;
    Payload payload;
};
```

`MessageId` 不是 wire 字段；当前实现直接复用 `seq_id`。
`seq_id` 规则见 [IPC_FRAMING.md](IPC_FRAMING.md)。

---

## 5. API

```cpp
class MessageManager
{
public:
    int assign_transport(std::unique_ptr<ITransport> transport);
    int assign_transport(ITransport& transport);
    int assign_transport(ITransport* transport);

    int start();
    void end();

    // client: 发 Call 并入队，立即返回 MessageId (>0)，失败 <0。
    // timeout_ms 只对 Request 生效；OneWay 不进入 pending，直接忽略。
    int send(uint16_t cmd, const Payload& payload,
             SendMode mode = SendMode::Request, int timeout_ms = -1);

    // client: 非阻塞检查 pending，立即返回。
    // >0      = MessageId，并取走 response
    // 0       = send 设置的 deadline 已过，pending 已移除
    // -AGAIN  = 还没到，下一轮 loop 再查
    // <0      = NOT_PENDING / CLOSED / INVALID
    int recv(MessageId id, Message* out);

    // client: 非阻塞消费 Notify；>0 = id，0 = 队列空
    int fetch_notify(Message* out);

    // server: 非阻塞消费 Call；>0 = id，0 = 队列空
    int fetch_request(Message* out);

    // server: 回复 Call。flag 是 wire 的 flag 字节。
    int reply(MessageId call_id, uint16_t cmd, const Payload& payload,
              uint8_t flag = 0);

    // server: 错误回复，payload 为 UTF-8 纯文本。
    int reply_error(MessageId call_id, uint16_t cmd,
                    uint8_t error_code, const std::string& message);

    // server: 向唯一 client 推事件，无 ACK。
    int notify(uint16_t cmd, const Payload& payload);

    // client: 放弃 pending Request；之后 recv 返回 NOT_PENDING。
    int discard(MessageId id);

    // 连接断开 / 致命错误；reason 是 DisconnectReason 的 int 值。
    int set_disconnect_callback(std::function<void(int reason)> callback);
};
```

模式约束：

```text
Client : send / recv / fetch_notify / discard
Server : fetch_request / reply / reply_error / notify
```

MM 不提供 callback dispatch；业务 dispatch 由上层 worker 通过
`fetch_request` / `fetch_notify` 完成。

---

## 6. 队列

### Client

```text
pending_requests : MessageId -> { deadline, response? }
notify_queue     : deque<Message>
outbound_queue   : deque<Frame>
```

### Server

```text
request_queue  : deque<Message>
outbound_queue : deque<Frame>
```

reader 分发：

```text
type=0 Call     -> request_queue
type=1 Response -> pending_requests[id]
type=2 Notify   -> notify_queue
```

`fetch_request` / `fetch_notify` 取走即消费，不需要 `discard`；
它们都是非阻塞的：`0` = 队列空，`<0` = 已关闭 / 错误。

---

## 7. Request / OneWay / discard

### Request

```cpp
// send 只入队，立即返回；deadline 由 send 保存。
MessageId id = mgr.send(NIC_GET_STATUS, req, SendMode::Request, 2000);

// recv 只查 pending，立即返回；等待由调用方的 loop 做。
Message resp;
for (;;)
{
    int rc = mgr.recv(id, &resp);
    if (rc > 0)
        break;                                      // 拿到 response
    if (rc == static_cast<int>(Status::AGAIN))
        continue;                                   // 下一轮 loop 再查
    if (rc == 0)
        break;                                      // timeout
    break;                                          // NOT_PENDING / CLOSED ...
}
```

- `send` 分配 `MessageId` 并把 deadline 写进 pending，立即返回；
- `recv` 只检查 pending map，不阻塞；
- response 已经 ready 时，即使 deadline 已过也优先返回 response；
- 返回 `0` 表示 deadline 已过，pending 已被移除，不需要再 `discard`。

### OneWay

```cpp
MessageId id = mgr.send(LOG_EVENT, payload, SendMode::OneWay);
// 不 recv
```

- 设置 `IPC_FLAG_NO_RESPONSE`；
- 不进入 pending map；
- `timeout_ms` 被忽略；
- `recv(id)` 返回 `NOT_PENDING`；
- `discard(id)` 返回 `NOT_FOUND`。

### discard

```cpp
int discard(MessageId id);
```

只处理 `send(Request)` 创建的 pending：

- pending 还在（不管 response 是否已到）：直接删除；
- 之后 `recv` 返回 `NOT_PENDING`；
- 已经 `recv` 消费：`NOT_FOUND`；
- OneWay：`NOT_FOUND`。

`discard` 不碰 notify queue、server request queue、outbound queue。

---

## 8. 队列满

### server outbound 满

`reply()` / `notify()` 入队失败时，不要阻塞 reader：

- Request：尽量回 `flag = ERROR`、`code = BUSY`、payload = `"outbound queue full"`；
- OneWay：直接丢弃；
- 连 error response 都入不了队：关闭连接。

不要无限增长 outbound queue。

### client incoming 满

`notify_queue` 满时按业务层策略处理（当前实现丢弃最旧的），不静默无限增长。

---

## 9. 线程模型

```text
reader loop:
    transport.wait(want_write, timeout)
    read  -> 增量 parser -> 校验 -> 分发到队列 / pending map
    write -> 从 outbound_queue 取 frame，处理部分写

上层 worker:
    fetch_request / fetch_notify   // 非阻塞，0 = 没消息
    业务 dispatch
```

- reader 不调用业务代码；
- 上层 worker 可以是 service main worker，也可以是 client worker；
- 不需要额外 dispatcher 线程；
- `send/reply/notify` 入 outbound queue 后调用 `transport.wake()`。

### 锁

```text
outbound_mutex  : outbound_queue
incoming_mutex  : request_queue / notify_queue
response_mutex  : pending_requests
state           : atomic running
```

规则：

- reader 只操作队列，不执行业务；
- `fetch_*` 在锁内 pop，出锁后再 dispatch；队列空返回 0；
- `recv` 只查 pending map，不等待；
- `discard` 只锁 pending map；
- `send/reply/notify` 只入 outbound queue；
- `end()`：`transport.shutdown()` → join reader → `transport.close()`。

---

## 10. Notify

MM 保留 `type=2`、`seq_id` bit15=1、`notify()` / `fetch_notify()` /
`notify_queue` 的实现；是否有 producer 由具体 profile 决定。

---

## 11. Profile

`magic` / `ver` / `cmd` / payload 的含义由 profile 定义；MM 不关心。
MM 不定义任何 command，包括 `SYS_*`。

- 通用模板：[IPC_COMMANDS.md](IPC_COMMANDS.md)
- ConnService 的真实 profile：[../conn-service/IPC_COMMANDS.md](../conn-service/IPC_COMMANDS.md)
