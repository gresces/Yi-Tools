# yi-pi-agent

把 Yi 事件总线接到 [pi](https://github.com/badlogic/pi) 编码代理上:消费
`pi-agent.*` 事件里的指令,调用 pi,再把输出发布回总线。

```
Yi  ──(pi-agent.*)──▶  yi-pi-agent  ──▶  pi --print ...
 ^                          │
 └──── pi-agent.result ◀────┘
```

单文件 C11,消费者和生产者一体;进程内一次只处理一条事件,其余排队。

## 事件约定

请求(任何 `types` 命中订阅的事件):

```json
{
  "id": "…",
  "source": "web",
  "types": ["pi-agent.run"],
  "time": "2026-11-16T12:00:00Z",
  "data": { "text": "用一句话总结今天的待办" }
}
```

指令字段依次尝试 `data.text`、`data.prompt`、`data.instruction`。

成功结果:

```json
{
  "source": "pi-agent",
  "types": ["pi-agent.result"],
  "data": {
    "request_id": "…",
    "request_source": "web",
    "request_types": ["pi-agent.run"],
    "prompt": "用一句话总结今天的待办",
    "output": "…",
    "exit_code": 0,
    "duration_ms": 2345,
    "truncated": false
  }
}
```

失败(pi 退出码非 0、超时、命令不存在)会发 `pi-agent.error`,`data` 里带
`error`、`stderr`、`exit_code`、`duration_ms`。

- 结果的 `id` 由 `source + type + 请求 id` 派生:事件被重投时重复发布会被
  Yi 去重,所以同一个请求不会产生两条结果。
- 结果发布成功才 ack 原事件;发布失败会 nack,等总线重投。
- 为防止自己消费自己,`result_type` / `error_type` 命中的事件会被直接 ack
  跳过——即使订阅模式是 `^pi-agent\.`。

## 构建与安装

```sh
make
make install
```

依赖 libcurl 和 OpenSSL;JSON 解析使用仓库内附带的
[jsmn](https://github.com/zserge/jsmn)(MIT)。

## 初始化

```sh
yi-pi-agent init
```

依次配置总线(写入 `~/.config/yi/bus.conf`)、消费者名称、订阅模式、pi 命令与
参数、工作目录、超时(写入 `~/.config/yi/pi-agent.conf`),最后可选部署为
systemd user 服务。

默认值:

| 键 | 默认 |
|---|---|
| `consumer` | `pi-agent` |
| `patterns` | `^pi-agent\.` |
| `command` | 存在 `~/.bun/bin/pi` 时用它,否则 `pi` |
| `args` | `--print --no-session --mode text` |
| `workdir` | `~/.local/state/yi-pi-agent/work` |
| `timeout` | `300` 秒 |

## 配置

```ini
# ~/.config/yi/pi-agent.conf
consumer = pi-agent
patterns = ^pi-agent\.
# types = pi-agent.run

command = /home/agent/.bun/bin/pi
args = --print --no-session --mode text
workdir = ~/.local/state/yi-pi-agent/work
timeout = 300
max_output = 20000
poll_timeout = 25
skip_history = true

result_type = pi-agent.result
error_type = pi-agent.error
source = pi-agent
```

`skip_history`(默认 `true`)只对**新建**的消费者生效:首次注册后先把总线里已有
的匹配事件逐条 ack 跳过,不执行它们,避免把历史请求重跑一遍(新消费者默认
会从活跃日志开头读)。消费者已存在(重启)时游标保留,不会 drain。

- `types` 是精确类型,`patterns` 是 Go RE2 正则,两者并集。
- `args` 按空白拆分,支持双引号;请求文本作为最后一个参数传给 pi。
- 执行时会把 `command` 所在目录拼到子进程 `PATH` 的最前面,所以
  `#!/usr/bin/env node` 这类包装脚本在 systemd 的精简 PATH 下也能找到同目录的
  `node`/解释器。
- `max_output` 是发布时保留的输出字节数,超出会截断并把 `truncated` 置真。
- `poll_timeout` 是长轮询秒数。

## 命令

```sh
yi-pi-agent            # 常驻
yi-pi-agent --once     # 只处理一轮(1 秒长轮询)后退出,方便测试
yi-pi-agent --check    # 打印配置(令牌隐藏),配置缺失也退出 0
```

## 部署

```sh
make install
systemctl --user daemon-reload
systemctl --user enable --now yi-pi-agent
journalctl --user -u yi-pi-agent -f
```

如果用户没有常驻登录,需要 `loginctl enable-linger <user>`,否则 user
service 会在最后一个会话结束时停止。

### 快速验证

```sh
# 发一条请求
curl -s -H "Authorization: Bearer $TOKEN" -H 'Content-Type: application/json' \
  -d '{"id":"01TESTPI0000000000000000000000","source":"cli",
       "types":["pi-agent.run"],"time":"2026-11-16T12:00:00Z",
       "data":{"text":"Reply with exactly: pong"}}' \
  http://127.0.0.1:9092/events

# 看结果
curl -s -H "Authorization: Bearer $TOKEN" \
  'http://127.0.0.1:9092/events?limit=5&type=pi-agent.result'
```

## 安全边界

- 总线事件可以驱动一个带 read/bash/edit/write 工具的编码代理,权限等同运行
  `yi-pi-agent` 的用户。请用专用低权限用户运行,不要用 root。
- 考虑在 `args` 里收紧工具(`--tools` / `--exclude-tools`)并固定 `workdir`;
  pi 自己的审批/沙箱设置同样生效。
- 消费者名固定后,重启会沿用服务端游标;不要在同一个总线上跑两个同名实例。
- 用 `consumer` 换一个新名字等于换一个消费者:首次启动会按 `skip_history`
  处理历史事件。
- 配置修改需要重启服务(`systemctl --user restart yi-pi-agent`),不热重载。
