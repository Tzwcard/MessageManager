# IPC Command Profile 样例

> **这是样例 / 模板，不是 MessageManager 的标准。**
>
> MessageManager 只负责 framing、队列、request/response 匹配和 Notify 投递。
> **它不定义、不解析、不特殊处理任何 `cmd`，包括 `SYS_*`。**
>
> 所有 command —— 包括 `SYS_PING` 这类看起来“系统级”的命令 ——
> 都由 **server / service profile** 自己定义、编号、校验和 dispatch。
> 具体 service 应该复制这份文件，改成自己的 command 表。

相关文档：

- [IPC_FRAMING.md](IPC_FRAMING.md)：wire framing
- [MESSAGE_MANAGER.md](MESSAGE_MANAGER.md)：配置 / API / 队列 / 线程模型
- [../conn-service/IPC_COMMANDS.md](../conn-service/IPC_COMMANDS.md)：ConnService 的真实 profile

---

## 1. 谁定义什么

| 层 | 负责 |
|---|---|
| MessageManager | length prefix、magic/ver、type、seq_id、flag/code、队列、匹配 |
| profile / service | `cmd` 表、payload schema、错误码含义、超时、OneWay / Notify 选择 |
| transport | 连接建立、非阻塞 read/write、wait/wake、close |

MM 不会因为 `cmd` 落在 `0x0001 - 0x00FF` 就自动处理；`SYS_*` 只是 profile
自己的命名习惯。service 也可以完全不用 `SYS_` 前缀，或者把 `0x0001`
定义成别的业务命令。

---

## 2. Profile 配置

```cpp
MessageManagerConfig config;
config.magic = {'E', 'X', 'A', 'M'};
config.version = 1;
config.mode = MMMode::Server;   // client 侧改成 MMMode::Client
```

```text
magic = "EXAM"
ver   = 1
cmd   = uint16_t，由本 profile 定义
payload = 不透明字节（本 profile 用 protobuf 或 raw text）
```

`magic` 区分 service，`ver` 固定 command 表 + payload schema。
不兼容的 command 集变更必须 bump `ver`。

---

## 3. command 表

```cpp
enum class Command : uint16_t
{
    // SYS：由本 service 自己定义，MM 不保留、不特殊处理
    SYS_PING            = 0x0001,

    // CFG
    CFG_GET             = 0x0101,
    CFG_SET             = 0x0102,

    // DATA
    DATA_READ           = 0x0201,
    DATA_WRITE          = 0x0202,
    DATA_CHANGED_EVENT  = 0x0203,   // Notify
};
```

```text
0x0000            reserved (GENERAL_ERROR)
0x0001 - 0x00FF   SYS       本 service 自定义
0x0100 - 0x01FF   CFG
0x0200 - 0x02FF   DATA
0x0300 - 0x7FFF   reserved
0x8000 - 0xFFFF   profile 自定义
```

- 只有 `0x0000` 是 framing 层保留值，profile 不能使用；
- `SYS_PING` 只是示例：由 service 的 dispatch loop 自己处理，
  MM 只负责把 Call 放进 `request_queue`，把 Response 带回 client。

---

## 4. 调用模式

| 模式 | wire | 用法 |
|---|---|---|
| Request / Response | Call + Response | `send(..., SendMode::Request, timeout)`，然后轮询 `recv` |
| OneWay | Call + `NO_RESPONSE` | `send(..., SendMode::OneWay)`，不 `recv` |
| Notify | Notify | server `notify()`，client `fetch_notify()` |

- `send` / `recv` / `fetch_*` 都是非阻塞的；
- timeout 只属于 Request/Response；OneWay 传入会被忽略；
- 完整代码见 [SAMPLE.md](SAMPLE.md)。

---

## 5. payload

| command | payload |
|---|---|
| `SYS_PING` | 空 |
| `CFG_GET` / `CFG_SET` | protobuf |
| `DATA_READ` / `DATA_WRITE` | protobuf |
| `DATA_CHANGED_EVENT` | protobuf |

也可以用 JSON / CBOR / raw text + splitter / 空 payload。
payload 格式错误由 profile 自己校验；MM 只当作不透明字节。

---

## 6. flag / code

`flag` / `code` 是两个独立字段：

```text
flag bit0    IPC_FLAG_NO_RESPONSE
flag bit1    IPC_FLAG_ERROR
flag bit2-7  reserved
code         flag & IPC_FLAG_ERROR 时为 error code，否则 0
```

- service 层错误：`cmd = request 的 cmd`、`flag = ERROR`、`code = error_code`；
- framing 错误：MM 回 `cmd = 0x0000`、`flag = ERROR`、`code = 具体错误`；
- 成功 response：`flag = 0`、`code = 0`。

完整规则见 [IPC_FRAMING.md](IPC_FRAMING.md)。

---

## 7. 错误码

`code` 只有 1 字节，最多 255 个错误码。`MessageDefs.h` 里的 `ErrorCode`
是常用集合，profile 可以扩展或重新解释。

```text
0 OK        1 INVALID_ARGUMENT   2 UNAVAILABLE       3 PERMISSION_DENIED
4 FAILED    5 TIMEOUT            6 NOT_FOUND         7 ALREADY_RUNNING
8 NOT_RUNNING  9 BUSY            10 NOT_SUPPORTED    11 UNSUPPORTED_VERSION
12 MAGIC_MISMATCH
```

更细的错误信息放在 error payload 里。

---

## 8. dispatch

server 就是 `switch (req.cmd)`；完整示例见 [SAMPLE.md](SAMPLE.md)。

`SYS_PING` 和业务命令在同一层 dispatch，没有任何“MM 内建命令”的特殊分支。

---

## 9. checklist

1. 选择 `magic` / `ver`，client 和 server 必须一致；
2. 编号 `cmd`，决定是否需要 `SYS_*` 分区；
3. 定义每个 `cmd` 的 payload schema；
4. 定义 Request / OneWay / Notify；
5. 定义 error code 和 error payload 格式；
6. 定义每个 command 的 timeout / retry 策略；
7. command 集不兼容变更时 bump `ver`；
8. MM 不会替你处理任何 command，包括 `SYS_PING`。
