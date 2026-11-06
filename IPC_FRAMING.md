# IPC Framing

> 一帧 = `[u32 size little-endian][serialized ipc_frame]`。
> `ipc_frame` 是 proto3 message，schema 在
> [proto/ipc_frame.proto](proto/ipc_frame.proto)。
>
> `magic` / `ver` / `command` / payload 的具体取值由 profile 定义。
>
> - [MESSAGE_MANAGER.md](MESSAGE_MANAGER.md)：配置 / API / 队列 / 线程模型
> - [IPC_COMMANDS.md](IPC_COMMANDS.md)：command profile 样例 / 模板

## 1. Wire layout

```text
[u32 size little-endian][ipc_frame bytes]
[u32 size little-endian][ipc_frame bytes]
...
```

- `size` 是 `ipc_frame` 序列化后的长度，不包含 4 字节前缀；
- `size` 必须满足 `1 <= size <= IPC_MAX_FRAME_SIZE`；
- `IPC_MAX_PAYLOAD = 1 MiB`；
- `IPC_MAX_FRAME_SIZE = IPC_MAX_PAYLOAD + 1024`；
- 没有 padding；
- 帧边界由 size 前缀保证。

```proto
syntax = "proto3";

package msgmgr.ipc;

message ipc_frame
{
    bytes  magic   = 1; // exactly 4 bytes
    uint32 ver     = 2; // uint8 range
    uint32 type    = 3; // uint8 range: 0=Call, 1=Response, 2=Notify
    uint32 command = 4; // uint16 range
    uint32 seq_id  = 5; // uint16 range
    uint32 flag    = 6; // uint8 range
    uint32 code    = 7; // uint8 range
    bytes  payload = 8; // opaque
}
```

## 2. 字段

### magic

- 库默认 `{0x20, 0x26, 0x11, 0x05}`；
- profile 可以覆盖，由 `MessageManagerConfig.magic` 配置，**两端必须一致**；
- wire 上是 `bytes`，长度必须正好 4；
- 不一致时回 `MAGIC_MISMATCH` error response（使用接收方自己的 magic/ver），
  然后关闭连接。

### ver

- 表示 **IPC protocol + command set 版本**，默认 `1`；
- `uint8` range，两端必须一致；
- 不兼容的 command 集变更必须 bump `ver`；
- 不一致时回 `UNSUPPORTED_VERSION` error，然后关闭。

### type

```text
0  Call       client -> service
1  Response   service -> client
2  Notify     service -> client，单向，无 ACK
```

### command

- `uint16` range，由 profile 定义；
- **MM 不定义、不解析、不特殊处理任何 command，包括 `SYS_*`**；
- request / response 使用同一个 `command`；
- `0x0000` 是 framing 层保留值 `GENERAL_ERROR`；
- profile 不得把 `0x0000` 分配给业务命令。

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
- `type` 与 bit15 不一致时按协议错误关闭。

### flag

```text
bit0    IPC_FLAG_NO_RESPONSE   OneWay Call，server 不应 reply
bit1    IPC_FLAG_ERROR         error response
bit2-7  reserved
```

### code

- `flag & IPC_FLAG_ERROR` 时是 error code（0..255）；
- 非 error 帧通常为 0，接收方不依赖；
- 成功 response 的 `code` 必须为 0；
- 队列满使用 `BUSY`。

### payload

- 对 framing 层是**不透明字节**，MM 不解析；
- 格式由 profile 和 `command` 决定（protobuf / JSON / CBOR / raw text 等）；
- 空 payload 合法；
- payload 长度不超过 `IPC_MAX_PAYLOAD`。

### 结构校验

接收方在 protobuf 解析之后校验：

```text
magic 正好 4 字节，且匹配配置
ver / type / flag / code 在 uint8 range 内
command / seq_id 在 uint16 range 内
type 合法，且与 seq_id bit15 一致
seq_id 不是 0x0000 / 0x8000（reserved）
Call / Notify 的 command 不是 0x0000（保留值）
payload <= IPC_MAX_PAYLOAD
```

失败时回 error（能解出字段时）然后关闭连接。
payload 格式错误由 profile 自己校验。

## 3. C/C++ 表示

- wire body 由 `msgmgr::ipc::ipc_frame` 生成；
- `src/ipc_framing.h` 里的 `detail::IpcHeader` 只是解码后的内部视图，
  不是 wire 布局；
- `EncodeFrame` 负责 protobuf 序列化 + 4 字节长度前缀；
- `FrameParser` 负责增量读取和 protobuf 解析；
- 不要把 `read()` 的临时 buffer 直接强转成任何 header / message。

## 4. 读帧

```text
1. 读满 4 字节 size
2. 校验 1 <= size <= IPC_MAX_FRAME_SIZE
3. 等到 buffer 里至少有 4 + size 字节
4. ParseFromArray 解析 ipc_frame
5. 校验字段 range / magic / ver / type / seq / command / payload
6. 按 type / command 分发
```

必须使用增量 parser：

- 一次 `read()` 可能只有半个 size 前缀；
- 可能只有 size + 部分 protobuf body；
- 可能一次包含多帧；
- parser 只有在 `available >= 4 + size` 时才产生完整 frame。

## 5. 错误帧

错误帧和正常帧使用同一套 `[size][ipc_frame]` 布局。

```text
magic mismatch:
  ver      = 接收方支持的版本
  type     = Response
  command  = 0x0000 (GENERAL_ERROR)
  seq_id   = request 的 seq_id（能解出时）
  flag     = IPC_FLAG_ERROR
  code     = MAGIC_MISMATCH
  payload  = "magic mismatch"

ver mismatch:
  ver      = 接收方支持的版本
  type     = Response
  command  = 0x0000 (GENERAL_ERROR)
  seq_id   = request 的 seq_id
  flag     = IPC_FLAG_ERROR
  code     = UNSUPPORTED_VERSION
  payload  = "unsupported version"

command error:
  type     = Response
  command  = request 的 command
  flag     = IPC_FLAG_ERROR
  code     = error_code
  payload  = UTF-8 纯文本错误信息
```

- framing 错误（magic / ver / type / seq / 长度 / protobuf）的 reply 固定用
  `command = 0x0000`；
- service 层错误（unknown command / BUSY / 业务错误）带回 request 的
  `command`；
- magic / ver 错误发送后关闭连接。
