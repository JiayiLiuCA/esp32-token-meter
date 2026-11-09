# Token Meter

把 T3 Code 用量页上的 Claude Code token 数和花费，显示在 Waveshare ESP32-S3-RLCD-4.2（4.2 寸 400×300 黑白反光屏）上。灵感来自旁边的 esp32-clawdmeter，但两者互相独立：clawdmeter 管额度，这里只管 token 和美元。

## 组成

- `firmware/`：板子固件（PlatformIO + Arduino）。蓝牙 BLE 外设，收到数据就重画屏幕。
- `host/tokenmeter.py`：电脑端后台程序。扫描 `~/.claude/projects` 下的对话记录，按 T3 Code 的规则去重、计价，一直在扫描蓝牙，板子一出现就连上、推送一次数据（板子收到后自己断开）。推送节奏由板子决定，见下面「省电」。
- `host/tokenmeter.service`：systemd user service，开机自动运行电脑端程序。

## 屏幕

左对齐、没有分隔线，四边都留 24 像素边距。

```
16.5B                         [===]
tokens  /  30 days

API cost          sessions
$11,660.76        837     [Clawd]
```

- token 数：特大号，130 像素高，单位 K/M/B 小一号，一位小数；放不下时缩成 110 像素（这一档从电池下方开始，可以用满宽度）。U8g2 自带的字库最大只有 92 像素，所以用 `tools/gen_bigfont.py` 从 Logisoso 原始字体（`tools/fonts/Logisoso.ttf`，GPL v2 + 字体例外，和 U8g2 的 logisoso 同源）生成，按墨迹裁切、统一字距。
- 说明行：`tokens / 30 days`，标出当前时间段（Past 24h / 7 days / 30 days / 90 days）。按 KEY 或 BOOT 键切到下一个（休眠时按键会先唤醒，约 1 秒内切换），断电后记得上次停在哪个。
- 底部两列：API cost（按 API 价格估算的花费）和 sessions（这段时间里有用量的不同对话个数）；标签和说明行用同一种字。
- 右上角：电池图标，上沿和数字顶端对齐，不显示百分比：22×11 的 1 像素细框，里面一条连续的电量条，分 0、1/3、2/3、满四档（≤10% 空、≤35% 1/3、≤75% 2/3、其余满），每次醒来读一次（插着电脑时每分钟一次）。电压取自 GPIO4（1/3 分压），3.00V 算 0%、4.12V 算 100%，同 Waveshare 示例。板子没有充电状态引脚，只插 USB、不装电池时会显示满格。
- 右下角：Clawd 风格的黑白像素小人（`firmware/src/mascot.*`，22×16 格、每格 2 像素即 44×32，造型参照 clawdmeter 的 Clawd，动作自己画）。用电池时每次醒来同步时播一个动作（插着电脑时每 10–15 秒一个），随机从这些里挑：眨眼、左右看、挥手、小跳、散步（往左走 16 像素、回头看数字、走回来）、思考（对话框里「...」逐个出现，然后「!」并跳一下）、跳舞（左右手轮流举起）、仰望大数字冒爱心、抖一抖。token 数（四个时间段任何一个）超过 30 分钟没增长就睡着（闭眼，时不时飘出 z z Z）；有新用量时跳一下并醒来。整屏重画约 22ms（CPU 降到 80MHz 省电）。
- 蓝牙断开不提示；只有超过 15 分钟没收到新数据时，说明行末尾加 `/ not updated`。

## 省电

用电池时板子大部分时间在深度休眠，屏幕在休眠中保持画面（屏幕切到低功耗刷新，复位脚和片选脚在休眠时锁住电平，醒来不重新初始化屏幕，所以不闪）。

- 同步节奏跟着用量走：最近 10 分钟内 token 数涨过（四个时间段任何一个变大都算），每 1 分钟醒来一次；超过 10 分钟没涨，改成每 5 分钟一次。只看 24 小时的数不够：24 小时之前那段用量更多时，正在用也可能变小；按日历算的时间段到半夜会往下掉。
- 定时醒来：打开蓝牙广播（最多 20 秒），电脑端连上写入一次数据后板子断开，重画屏幕，小人播一个动作（有新用量就跳一下），回到静止姿势后再睡。电脑端没在运行时，20 秒后照样去睡，15 分钟后说明行出现 `/ not updated`。
- KEY / BOOT 唤醒：切换时间段，最后一次按键后再醒 5 秒，然后接着睡到原定的下次同步时间，不打乱同步节奏。
- 插着电脑 USB（检测到 USB 数据帧，单纯的充电头不算）时不休眠：小人照常每 10–15 秒动一次，固定每 1 分钟同步（不耗电池），串口一直可用，可以直接烧录。
- CPU 固定 80MHz（蓝牙允许的最低频率），整屏重画约 22ms。
- 估算（2600 mAh 电池，没有实测）：最早不休眠时约 40–60 mA、2–3 天；固定每 5 分钟同步时估计 1.5–3 个月；现在有用量时每 1 分钟同步，按每天用 8 小时算估计 1–2 个月。每次醒来约 3–5 秒，耗电主要在这几秒。

板子休眠时串口不在，电脑认不出它。要烧录时插上电脑 USB 等它下次醒来（最多 5 分钟，有用量时最多 1 分钟），或者按一下 KEY 唤醒；醒来后检测到电脑就不再睡。


## 和 T3 Code 的对齐方式

规则照搬 T3 Code 桌面版里的 `claudeUsage.ts` 和 `usagePricing.ts`：

- 只算 `type == "assistant"` 且带 `message.usage` 的记录，跳过 `<synthetic>`；advisor 的调用单独记一条。
- 按 `message.id:requestId` 去重，整次扫描里第一次出现的为准。
- token = input + cache_read + cache_creation + output。
- 价格用 T3 Code 自己维护的价格表 `~/.t3/userdata/usage-model-rates.json`；fast 模式按价格表里的倍数算。
- sessions = 时间段内被计入的记录里不同 sessionId 的个数，同 T3 Code `distinctSessions`。
- 时间段（T3 Code 前端 `session-logic` 里的 `ct`）：24H = 从当前分钟往前推 24 小时；7D / 30D / 90D = 按本地日历算，含今天，比如 7D 是今天加前 6 天。
- Claude Code 默认删掉 30 天前的对话记录，T3 Code 在 `usage-scan-cache-v11.json` 里保留了这些文件的用量。电脑端程序把缓存中“磁盘上已不存在的文件”的记录合并进来，否则 30D / 90D 会偏少。

已核对：2026-11-07 全天 207,100,813 token，与 T3 Code 缓存完全一致；90D 截至同一时刻本程序 20,244,581,758 token、T3 Code 缓存 20,244,578,796 token，相差 2,962（两边读取时间点不同）。

## 常用命令

```sh
# 打印当前数字（不连蓝牙）
uv run host/tokenmeter.py --print

# 编译并烧录固件
cd firmware && pio run -t upload --upload-port /dev/ttyACM0

# 安装开机自启动
ln -sf "$PWD/host/tokenmeter.service" ~/.config/systemd/user/tokenmeter.service
systemctl --user daemon-reload && systemctl --user enable --now tokenmeter
journalctl --user -u tokenmeter -f      # 看日志
```

## 蓝牙协议

- 设备名 `TokenMeter`，服务 UUID `7e1a0001-6b2c-4d3e-9f10-746f6b6d7472`。
- 写入特征 `7e1a0002-6b2c-4d3e-9f10-746f6b6d7472`：每次写一个完整 JSON，最大 512 字节（BLE long write）。
- 板子只在同步窗口里广播（有用量时每 1 分钟一次，否则每 5 分钟一次，每次最多 20 秒），收到一次写入后主动断开。电脑端推送后停 40 秒再扫描，赶在板子下一次醒来之前。

```json
{"v":4,"p":[[60849926,5723,6],[3470333917,235226,133],[16452525736,1166076,837],[20261684674,1384018,1249]]}
```

| 字段 | 含义 |
| --- | --- |
| `v` | 协议版本，目前是 4 |
| `p` | 4 个时间段，依次为 24H、7D、30D、90D，每项 `[token 数, 花费美分, sessions 个数]` |

## 硬件引脚

来自 Waveshare 出厂固件 `examples/factory/main/user_config.h`：SCK 11、MOSI 12、DC 5、CS 40、RST 41、TE 6；KEY 18、BOOT 0。屏幕驱动 `ST7305_U8g2.*` 拷自 Waveshare 官方示例（Apache 2.0）。
