# yi-mail

轮询多个 IMAP 邮箱,把新邮件写成 Yi 的 `mail.received` 事件。单文件 C11,
依赖 libc、OpenSSL、libcurl(iconv 来自 glibc)。

```json
{
  "id": "0123456789abcdef0123456789abcdef",
  "source": "work",
  "types": ["mail.received"],
  "time": "2026-01-02T03:04:05Z",
  "data": {
    "account": "work",
    "folder": "INBOX",
    "uid": 103,
    "uidvalidity": 7,
    "size": 164,
    "from": "Carol <carol@example.com>",
    "subject": "示例主题",
    "date": "Fri, 2 Jan 2026 11:04:05 +0800",
    "message_id": "<a103@example.com>"
  }
}
```

- `source` 默认是账户名,可在账户段里覆盖。
- `time` 取邮件 `Date` 头(转 UTC),解析失败才用当前时间。
- `id` 是 `(source, folder, uidvalidity, uid)` 的 SHA-256 前 32 位;发布成功
  但状态没落盘时重发会被 Yi 去重。
- 只取头部,不下载正文和附件;主题做 RFC 2047 解码(B/Q)+ iconv 转 UTF-8。
- `data.text` 就是主题,前端内容列直接显示;空主题写 `(无主题)`。

## 构建

```sh
make
make install
```

需要 OpenSSL 和 libcurl 的开发头文件,不需要 C++。

## 初始化

```sh
yi-mail init
```

先配置总线(协议、主机、端口、令牌,写入 `~/.config/yi/bus.conf`),然后可以
交互式添加 IMAP 账户(名称、主机、端口、TLS 方式、用户名、授权码、来源、
文件夹),写入 `~/.config/yi/mail.conf`。已存在的账户会被保留;如果旧配置里还
留着 `url`/`token`,`init` 会把它们迁移到 `bus.conf` 并删掉旧键,其他全局设置
(如 `interval`、`max_fetch`)不变。

最后询问 `部署为 systemd user 服务? [y/N]: `。回答是时,会自动把二进制复制到
`~/.local/bin`、写入 `~/.config/systemd/user/yi-mail.service`、执行
`daemon-reload`、`enable` 和 `restart`;回答否(默认)则只写配置,之后手动:

```sh
systemctl --user daemon-reload
systemctl --user enable --now yi-mail
journalctl --user -u yi-mail -f
```

非交互/脚本场景可以从 stdin 喂答案:

```sh
# 协议/主机/端口/令牌,不添加账户、不部署服务
printf 'http\n127.0.0.1\n9092\n%s\nn\nn\n' "$TOKEN" | yi-mail init
```

## 配置

```ini
# ~/.config/yi/mail.conf
[account:work]
host = imap.example.com
port = 993
tls = implicit          # implicit | starttls | none
tls_verify = true
user = user@example.com
password = 应用专用密码
source = work
folders = INBOX         # 逗号分隔
```

账号段必填 `host`、`user`、`password`;端口不写时 implicit 用 993,其余用 143。
`tls_verify = false` 只给自签服务器或本地测试用。所有邮箱都要先在网页端开启
IMAP;Gmail/QQ/163 使用**授权码/应用专用密码**,不是登录密码。本工具只支持
密码认证,不做 OAuth。

登录后如果服务器声明了 `ID`(RFC 2971),会自动发一次 `ID`,163/126 不做这
一步会返回 `Unsafe Login`。

## 新邮件的判定

用 IMAP 的 UID 游标,不用 `\Seen`:

1. `SELECT` 文件夹,拿到 `UIDVALIDITY`、`UIDNEXT`。
2. 状态文件按 `账号 + 文件夹` 记录 `uidvalidity` 和 `last_uid`。
3. 首次见到文件夹只记基线(或 `backfill = true` 时补发最近 `max_fetch` 封)。
4. 之后 `UID FETCH (last_uid+1):*`,UID 不大于游标的响应会被跳过。
5. `UIDVALIDITY` 变化说明服务端重排 UID:重置游标,不重放。
6. 落后超过 `max_fetch` 封时只取最新窗口,并在日志里说明。

状态文件默认 `~/.local/state/yi-mail/state.tsv`,临时文件 + rename 写入。
删掉它等于重新认基线。

## 命令

```sh
yi-mail --once         # 轮询一轮就退出
yi-mail                # 常驻,每个周期重读配置
yi-mail --check        # 打印生效配置(令牌/密码隐藏)
```

配置是每轮重读的:加账号、改密码不用重启。没有配置账号时进程会等下一轮,
unit 保持 `active (running)`;配置有语法错误时保留上一份继续跑。

## 验证

```sh
curl -s -H "Authorization: Bearer <token>" \
  'http://127.0.0.1:9092/events?limit=10&type=mail.received'
```

## 边界

- 轮询而非 IMAP IDLE:实现简单、对服务器友好,新邮件最多延迟一个 `interval`
  (默认 60 秒,最小 5 秒)。
- 只看头部,不判断已读/未读,不解析 MIME 分片;无法解码的字节原样保留。
- 不做 OAuth(只给 OAuth 的邮箱用不了)。
- 同一台机器跑多个实例时,给每个实例单独的 `--config` 和 `state`,否则游标会打架。
