# 工具约定

这个仓库收的是一组独立的小工具。它们可以语言不同、依赖不同,但对外行为要
一致,这样换一台机器、换一个工具都不需要重新学。

## 必须支持 `init`

每个工具都必须是:

```sh
yi-<tool> init
```

一条命令完成接入配置:

1. 交互式询问 Yi 总线的 **协议 / 主机 / 端口 / 令牌**,方括号里是当前值,
   直接回车保留。
2. 令牌输入默认不回显(终端下关闭 echo)。
3. 写完配置后做一次 `GET <scheme>://<host>:<port>/healthz` 探测:
   - 成功:打印 `ok`,继续;
   - 失败:打印原因,并在交互模式下询问是否仍然保存。
4. 把结果原子写入 `~/.config/yi/bus.conf`(权限 `0600`),打印路径。
5. 继续询问该工具自己的配置项,写入自己的配置文件。
6. 询问 `部署为 systemd user 服务? [y/N]: `(默认否,非终端 EOF 也是否)。
   回答是时:
   - 如果当前可执行文件不在 `~/.local/bin/<bin>`,先复制过去(`0755`);
   - 生成并写入 `~/.config/systemd/user/<bin>.service`(`0644`,原子写);
   - 依次执行 `systemctl --user daemon-reload`、`enable <unit>`、`restart <unit>`;
   - 任一步失败都打印命令、退出码和 `journalctl --user -u <unit>` 提示,并以 `1` 退出
     (配置文件此时已经写好)。
7. 最后打印下一步提示(部署成功则提示看日志,否则提示 `systemctl --user enable --now ...`)。

`init` 不要求 root,不读写仓库目录,也不需要预先存在配置文件。

### 非交互场景

stdin 不是终端时,`init` 从 stdin 逐行读取答案(空行 = 保留默认),EOF 后
用默认值继续;不询问"是否仍然保存",探测失败只警告。这样部署脚本可以:

```sh
# 协议 / 主机 / 端口 / 令牌,然后回答“是否添加账户”和“是否部署服务”
printf 'http\n127.0.0.1\n9092\n%s\nn\nn\n' "$TOKEN" | yi-mail init
```

### 退出码

| 码 | 含义 |
|---|---|
| `0` | 配置写入成功 |
| `1` | 写文件失败、交互中放弃保存,或服务部署失败 |
| `2` | 参数用法错误 |

## 配置文件

所有配置都在 `~/.config/yi/` 下(遵循 `$XDG_CONFIG_HOME`),**不进仓库**。

### `bus.conf`(共享)

```ini
# ~/.config/yi/bus.conf
scheme = http
host = 127.0.0.1
port = 9092
token = 你的令牌
```

- `scheme` 默认 `http`,反向代理 + TLS 时写 `https`。
- 也接受 `url = https://yi.example.com` 整体覆盖 scheme/host/port。
- 缺省值:`http://127.0.0.1:9092`,空令牌。
- 解析时忽略不认识的键,方便以后扩展。

### 优先级

从高到低:

1. 命令行 `--url` / `--token`
2. 环境变量 `YI_URL` / `YI_TOKEN`
3. `~/.config/yi/bus.conf`
4. 工具自己的旧配置(`url` / `token`,已废弃,仅为兼容保留)
5. 内置默认值

### 工具配置

每个工具一个文件,只放自己的东西:

| 工具 | 文件 | 内容 |
|---|---|---|
| `yi-niri` | `~/.config/yi/niri.conf` | `source`、`timeout_ms` |
| `yi-mail` | `~/.config/yi/mail.conf` | `[account:名字]` 段与 `interval` 等全局项 |

旧版的 `url` / `token` 如果还留在工具配置里,`init` 会把它们迁移到 `bus.conf`
并删掉旧键,账户和其他全局设置保持不变。

## 隐私

- 仓库里**不允许**出现任何真实地址、令牌、邮箱、密码、主机名。
- 示例一律用 `127.0.0.1`、`yi.example.com`、`<token>`、`user@example.com`。
- 配置文件权限 `0600`;日志和 `--check` 输出不打印令牌与密码。
- 提交前自查:`grep -rn "token" .` 只应看到占位符。

## 安装

每个工具目录都有独立的 `Makefile`,顶层 `make` 只是依次调用:

```sh
make            # 构建全部
make install    # 安装二进制到 ~/.local/bin,unit 到 ~/.config/systemd/user
make test
make clean
```

语言自带包管理器时(pip / cargo / npm),在工具自己的 README 里写清楚,
但命令名和 `init` 行为保持一致。
