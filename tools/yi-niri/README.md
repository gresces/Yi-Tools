# yi-niri

把 niri 会话的关键时刻写进 Yi:

| 时刻 | 事件类型 |
|---|---|
| 会话启动 | `system.boot` |
| 系统关机 / 重启前 | `system.shutdown` |
| 锁屏 | `system.lock` |
| 解锁 | `system.unlock` |

事件 `source` 由 `init` 配置(默认 `niri`),`data` 带 `host`、`user`、`session`、
`kernel`、`boot_id`、`niri` 版本和 `uptime_sec`;`system.unlock` 额外带
`locked_for_sec`,`system.shutdown` 额外带 `reason`。
`data.text` 是给前端内容列的一行摘要(`开机` / `关机` / `锁屏` / `解锁`)。

## 为什么是 logind

niri 的 `niri msg event-stream` 没有锁屏事件,但 niri 实现了
`ext-session-lock-v1`,锁定/解锁时会调用 logind 的 `Session.SetLockedHint`。
所以 `LockedHint` 才是权威状态,换任何走 `ext-session-lock` 的锁屏工具都能用。
关机同理:logind 在整机下电前广播 `PrepareForShutdown`。

## 依赖与构建

- `libsystemd`(sd-bus)
- `libcurl`
- `nlohmann/json`
- Yi 仓库的 `lib/cpp/yi.hpp`(默认 `../../../Yi/lib/cpp`)

```sh
make
make install           # 二进制 + systemd user unit
```

## 初始化

```sh
yi-niri init
```

依次询问总线协议、主机、端口、令牌(终端下不回显),写 `~/.config/yi/bus.conf`;
再询问事件来源和请求超时,写 `~/.config/yi/niri.conf`。

最后询问 `部署为 systemd user 服务? [y/N]: `。回答是时,会自动把二进制复制到
`~/.local/bin`、写入 `~/.config/systemd/user/yi-niri.service`、执行
`daemon-reload`、`enable` 和 `restart`;回答否(默认)则只写配置,之后手动:

```sh
systemctl --user daemon-reload
systemctl --user enable --now yi-niri
journalctl --user -u yi-niri -f
```

也可以通过环境变量或参数临时覆盖:

```sh
YI_URL=http://127.0.0.1:9092 YI_TOKEN=<token> yi-niri run
yi-niri --url http://127.0.0.1:9092 --token <token> run
```

配置优先级见 [docs/init.md](../../docs/init.md)。

## 命令

```sh
yi-niri run        # 常驻:启动时发 boot,之后跟随 logind
yi-niri boot       # 手动发一条 system.boot
yi-niri lock       # 手动发一条 system.lock
yi-niri unlock     # 手动发一条 system.unlock
yi-niri shutdown   # 手动发一条 system.shutdown
yi-niri --check    # 打印生效配置(令牌隐藏)
```

`yi-niri boot` 这类一次性命令适合挂到 niri 按键或脚本里。正常使用
`swaylock` 时不需要手动补 `lock`,logind 会给出状态。

## 安装为 user service

`make install` 会把 `yi-niri.service` 放到 `~/.config/systemd/user/`。
unit 是 `WantedBy=niri.service` + `PartOf=niri.service`:niri 启动它才启动,
niri 退出时收到 `SIGTERM`,顺手发出 `system.shutdown`(原因 `agent-stop`)。

## 验证

```sh
curl -s -H "Authorization: Bearer <token>" \
  'http://127.0.0.1:9092/events?limit=10&type=system.lock'
```

## 边界

- **开机**:事件在图形会话启动(agent 启动)时发出,不覆盖登录前的内核启动。
- **关机**:依赖 logind 的 `PrepareForShutdown`,不申请 shutdown inhibitor;
  Yi 恰好不在线时这条事件会丢,这是为了避免阻塞关机。
- **锁屏**:只认 `LockedHint`,锁屏工具必须走 `ext-session-lock-v1`
  (swaylock 1.8+、gtklock、waylock 都行)。用 layer-shell 假装锁屏的工具
  需要手动补事件。
- 不要把同一个 agent 跑两份,否则 boot/shutdown 会重复。
