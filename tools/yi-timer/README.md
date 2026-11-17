# yi-timer

按日历时间向 Yi 发布定时器事件。单文件 C11,一个常驻进程,没有线程、没有
轮询数据库,空闲时只在绝对时间点醒来。

```json
{
  "id": "0123456789abcdef0123456789abcdef",
  "source": "timer",
  "types": ["timer.hourly"],
  "time": "2026-11-16T02:00:00Z",
  "data": {
    "name": "hourly",
    "schedule": "every 1h",
    "planned": "2026-11-16T02:00:00Z",
    "fired": "2026-11-16T02:00:01Z",
    "late_sec": 1,
    "seq": 42,
    "host": "example-host",
    "yi-event-life": "2026-11-16T08:00:00Z"
  }
}
```

- `time` 和 `planned` 是计划触发时刻(UTC);`fired` 是实际发布时刻,
  `late_sec` 是两者之差。
- `id` 由 `source + name + planned` 的 SHA-256 派生,重复发布会被 Yi 去重。
- `seq` 是进程启动以来的第几次触发,重启后从 0 开始。
- `yi-event-life` 是事件的过期时间(UTC),等于 `planned + 6 × 周期`:`every N`
  是 6N 秒,`daily` 是 6 天,`weekly` 是 42 天。
- `data.text` 是本地时间的触发时刻(`YYYY-MM-DD HH:MM:SS`),供前端内容列显示。

## 构建与安装

```sh
make
make install
```

依赖 libcurl 和 OpenSSL(SHA-256),不依赖 C++。

## 初始化

```sh
yi-timer init
```

先配置总线(协议、主机、端口、令牌,写入 `~/.config/yi/bus.conf`),然后可以
交互式添加定时器(名称、计划、类型、来源),写入 `~/.config/yi/timers.conf`。
已存在的定时器会被保留。最后询问是否部署为 systemd user 服务:

```sh
systemctl --user enable --now yi-timer
journalctl --user -u yi-timer -f
```

非交互/脚本场景:

```sh
# 协议/主机/端口/令牌,不添加定时器、不部署服务
printf 'http\n127.0.0.1\n9092\n%s\nn\nn\n' "$TOKEN" | yi-timer init
```

## 配置

```ini
# ~/.config/yi/timers.conf
max_late = 3600

[timer:hourly]
schedule = every 1h
type = timer.hourly      # 默认 timer.fired
source = timer           # 默认 timer
```

| 键 | 必填 | 说明 |
|---|---|---|
| `schedule` | 是 | 见下面的语法 |
| `type` | 否 | 事件类型,默认 `timer.fired` |
| `source` | 否 | 事件来源,默认 `timer` |
| `max_late` | 否 | 全局;迟到超过这么多秒就跳过本期,`0` 表示总是补发,默认 `3600` |

### schedule 语法

| 写法 | 含义 |
|---|---|
| `every 90s` | 每 90 秒(从当地 0 点起算) |
| `every 30m` | 每 30 分钟,即 :00 和 :30 |
| `every 1h` | 每小时整点 |
| `daily 09:00` | 每天 09:00(本地时间) |
| `daily 21:30:15` | 每天 21:30:15 |
| `weekly fri 18:00` | 每周五 18:00(本地时间) |

`every` 支持 `s`/`m`/`h`,上限 24 小时,锚定当地 0 点;`daily`/`weekly`
用本地时区,跨夏令时会按系统时区规则落到实际时刻。

## 命令

```sh
yi-timer            # 常驻
yi-timer --check    # 打印总线配置和每个定时器的下一次触发时间
yi-timer --help
```

配置每 30 秒内会被重读(定时器进程最多睡 30 秒就会醒来一次),新增或修改
定时器不需要重启;删掉的定时器会停止触发。`timers.conf` 不存在时进程会
保持运行并等待它出现,不会反复重启。

## 精度与资源

- 用 `clock_nanosleep(CLOCK_REALTIME, TIMER_ABSTIME)` 睡到计划时刻,
  触发精度通常在几十毫秒,满足 ±5 秒的要求。
- 空闲时每个进程只占一个定时唤醒(最长 30 秒一次),CPU 基本为 0;
  内存主要是 libcurl/OpenSSL 的常驻部分。
- 发布是同步 HTTP;如果 Yi 不在线,会记日志并在该次尝试后继续下一个周期。

## 边界

- 不持久化触发状态:进程重启后不会补发停机期间错过的定时器,只会从
  当前时间之后重新排期。
- 系统休眠唤醒后会检查计划时刻:迟到不超过 `max_late` 补发一次
  (只补最近一期,不会把错过的每一期都发出来),超过就跳过并记日志。
- `every` 锚定当地 0 点,所以 `every 1h` 是整点;不接受超过 24 小时或
  小于 1 秒的间隔,更长周期请用 `daily`/`weekly`。
- 时区使用系统本地时区;运行中改时区或跨夏令时的边界由 mktime 处理。
