# IPC Framing

> 本文只定义 framing：header、字段、读帧和协议错误。
> `magic` / `ver` / `cmd` / payload 的具体取值由 profile 定义。
>
> - [MESSAGE_MANAGER.md](MESSAGE_MANAGER.md)：配置 / API / 队列 / 线程模型
> - [IPC_COMMANDS.md](IPC_COMMANDS.md)：command profile 样例 / 模板

多字节整数统一 **little-endian**。

## 1. Wire layout

```text
offset  size  field        说明
0       4     magic[4]     service 标识
4       1     ver          IPC protocol + command set 版本
5       1     type         0=Call, 1=Response, 2=Notify
6       2     cmd          uint16
8       2     seq_id       uint16
10      1     flag         IPC_FLAG_*
11      1     code         flag & IPC_FLAG_ERROR 时为 error code，否则 0
12      4     sz_payload   payload 长度
16      N     payload      不透明字节
```

```text
frame_size = 16 + sz_payload
```

- `IPC_MAX_PAYLOAD = 1 MiB`；
- `sz_payload` 只表示 payload 长度，不包含 header；
- `sz_payload == 0` 合法；
- 没有 padding；
- frame 边界由 `sz_payload` 保证。

## 2. 字段

### magic

- 库默认 `{0x20, 0x26, 0x11, 0x05}`；
- profile 可以覆盖，由 `MessageManagerConfig.magic` 配置，**两端必须一致**；
- 接收方每帧校验；
- 不一致时回 `MAGIC_MISMATCH` error response（使用接收方自己的 magic/ver），
  然后关闭连接，不解析 payload。

### ver

- 表示 **IPC protocol + command set 版本**，默认 `1`；
- 两端必须一致；`cmd` 表、payload schema、flag/code 语义都属于该 `ver`；
- 不兼容的 command 集变更必须 bump `ver`；
- 不一致时回 `UNSUPPORTED_VERSION` error，然后关闭；
- `magic` / `ver` 匹配后不再做 handshake。

### type

```text
0  Call       client -> service
1  Response   service -> client
2  Notify     service -> client，单向，无 ACK
```

### cmd

- `uint16_t`，由 profile 定义；
- **MM 不定义、不解析、不特殊处理任何 `cmd`，包括 `SYS_*`**；
- request / response 使用同一个 `cmd`；
- Notify 的 `cmd` 也由 profile 定义；
- `0x0000` 是 framing 层保留值 `GENERAL_ERROR`：
  - framing 错误的 error reply 固定使用 `cmd = 0x0000`；
  - profile 不得把它分配给业务命令；

### seq_id

```text
bit15 = 0 : Call / Response
bit15 = 1 : Notify

0x0000  reserved
0x8000  reserved

Call / Response : 0x0001 .. 0x7FFF
Notify          : 0x8001 .. 0xFFFF
```

- Response 原样带回 Call 的 `seq_id`；
- Notify 使用 server 自己的 seq 空间；
- `type` 是权威字段，bit15 用于快速分流和校验；
- `type` 与 bit15 不一致时按协议错误关闭。

### flag

```text
bit0    IPC_FLAG_NO_RESPONSE   OneWay Call，server 不应 reply
bit1    IPC_FLAG_ERROR         error response
bit2-7  reserved
```

### code

- 1 字节；
- `flag & IPC_FLAG_ERROR` 时是 error code（0..255）；
- 非 error 帧通常为 0，接收方不依赖；
- 成功 response 的 `code` 必须为 0；
- 队列满使用 `BUSY`。

### payload

- 对 framing 层是**不透明字节**，MM 不解析；
- 格式由 profile 和 `cmd` 决定（protobuf / JSON / CBOR / raw text 等）；
- 外层 `sz_payload` 已给出长度，内层不需要再套 length-delimited；
- 空 payload 合法。

### 结构校验

接收方只做结构性校验：

```text
magic 匹配
ver 匹配
type 合法，且与 seq_id bit15 一致
seq_id 不是 0x0000 / 0x8000（reserved）
Call / Notify 的 cmd 不是 0x0000（保留值）
sz_payload <= IPC_MAX_PAYLOAD
```

失败时回 error（header 可读时）然后关闭连接。
payload 格式错误由 profile 自己校验。

## 3. C/C++ 表示

```cpp
struct IPC_HEADER
{
    uint8_t  magic[4];
    uint8_t  ver;
    uint8_t  type;
    uint16_t cmd;
    uint16_t seq_id;
    uint8_t  flag;
    uint8_t  code;
    uint32_t sz_payload;
};

static_assert(sizeof(IPC_HEADER) == 16);
```

整帧分配：

```cpp
const size_t frame_size = sizeof(IPC_HEADER) + hdr.sz_payload;
```

- 不要用 `sizeof(IPC_MESSAGE) + sz_payload`；
- 不要把 `read()` 的临时 buffer 直接强转成 header / message；
- 必须由增量 parser 拼够一整帧后再解析。

## 4. 读帧

```text
1. 读满 16 字节 header
2. 校验 magic
3. 校验 ver
4. 校验 type / seq_id 一致性
5. 校验 sz_payload <= 1 MiB
6. 等到 buffer 里至少有 16 + sz_payload 字节
7. 取出 payload
8. 按 type / cmd 分发
```

必须使用增量 parser：

- 一次 `read()` 可能只有半个 header；
- 可能只有 header + 部分 payload；
- 可能一次包含多帧；
- parser 只有在 `available >= 16 + sz_payload` 时才产生完整 frame。

## 5. 错误帧

错误帧和正常帧使用同一套 header + payload 布局。

```text
magic mismatch:
  ver      = 接收方支持的版本
  type     = Response
  cmd      = 0x0000 (GENERAL_ERROR)
  seq_id   = request 的 seq_id（header 可读时）
  flag     = IPC_FLAG_ERROR
  code     = MAGIC_MISMATCH
  payload  = "magic mismatch"

ver mismatch:
  ver      = 接收方支持的版本
  type     = Response
  cmd      = 0x0000 (GENERAL_ERROR)
  seq_id   = request 的 seq_id
  flag     = IPC_FLAG_ERROR
  code     = UNSUPPORTED_VERSION
  payload  = "unsupported version"

command error:
  type     = Response
  cmd      = request 的 cmd
  flag     = IPC_FLAG_ERROR
  code     = error_code
  payload  = UTF-8 纯文本错误信息
```

- framing 错误（magic / ver / type / seq / 长度）的 reply 固定用
  `cmd = 0x0000`；
- service 层错误（unknown command / BUSY / 业务错误）带回 request 的 `cmd`；
- magic / ver 错误发送后关闭连接；
- 成功 response 的 `code` 为 0。
