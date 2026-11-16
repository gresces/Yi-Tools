# Yi-Tools

一组围绕 [Yi](../Yi) 的小工具。每个工具都能用标准工具链构建、安装,并且都
支持同一条接入命令:

```sh
yi-<tool> init
```

它会交互式询问 Yi 总线的协议、主机、端口和令牌,写入 `~/.config/yi/bus.conf`,
然后补全该工具自己的配置。

| 工具 | 作用 | 语言 | 文档 |
|---|---|---|---|
| `yi-niri` | niri 会话的 boot / lock / unlock / shutdown | C++17 | [tools/yi-niri](tools/yi-niri/README.md) |
| `yi-mail` | 多个 IMAP 邮箱的新邮件 | C11 | [tools/yi-mail](tools/yi-mail/README.md) |
| `yi-timer` | 按小时/每天/每周发布定时器事件 | C11 | [tools/yi-timer](tools/yi-timer/README.md) |
| `yi-pi-agent` | 消费 pi-agent.* 指令,调用 pi 并发布结果 | C11 | [tools/yi-pi-agent](tools/yi-pi-agent/README.md) |

```
Makefile                     顶层入口,依次调用各工具
docs/init.md                 工具如何实现 init、配置格式与优先级
tools/yi-niri/               niri 事件生产者 + systemd user unit
tools/yi-mail/               IMAP 轮询生产者 + systemd user unit
tools/yi-timer/              日历定时器生产者 + systemd user unit
tools/yi-pi-agent/           pi 消费者 + 结果生产者 + systemd user unit
```

`yi-niri` 需要 Yi 仓库里的 `lib/cpp/yi.hpp`,默认按 `../../../Yi/lib/cpp` 查找;
不在这个位置时用 `make YI_CPP=/path/to/Yi/lib/cpp`。

## 快速开始

```sh
make                 # 构建全部工具

yi-niri init   # 配置总线;最后可选部署并启动 systemd 服务
yi-mail init   # 同上,并可顺便添加 IMAP 账户
yi-timer init  # 同上,并可顺便添加定时器
yi-pi-agent init  # 配置 pi 消费/生产
```

`init` 会问 `部署为 systemd user 服务? [y/N]`。回答是时它会自动把二进制装到
`~/.local/bin`、生成 unit、`daemon-reload`、`enable` 并 `restart`,不需要再
手动敲 systemctl。回答否(默认)则只写配置。

手动安装/启用仍然可用:

```sh
make install                                  # 二进制 + unit
systemctl --user enable --now yi-niri
systemctl --user enable --now yi-mail
systemctl --user enable --now yi-timer
systemctl --user enable --now yi-pi-agent
```

只想装一个:

```sh
make yi-niri        # 或 make yi-mail / yi-timer
make -C tools/yi-mail install
```

## 配置

所有配置都在 `~/.config/yi/` 下,权限 `0600`,**不放进仓库**:

- `bus.conf`:共享的总线连接信息(`scheme`/`host`/`port`/`token`)
- `niri.conf`:niri 事件来源与超时
- `mail.conf`:IMAP 账户

优先级:命令行 > 环境变量(`YI_URL`/`YI_TOKEN`)> `bus.conf` > 内置默认值
(默认 `http://127.0.0.1:9092`、空令牌)。细节见 [docs/init.md](docs/init.md)。

## 隐私

仓库里不包含任何真实地址、令牌、邮箱或密码;示例统一使用 `127.0.0.1`、
`example.com` 和 `<token>` 这类占位符。提交前可以自查:

```sh
grep -rn "token =\|password =\|Authorization" .
```
