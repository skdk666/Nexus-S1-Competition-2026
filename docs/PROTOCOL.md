# USB CDC / HID

Agent 是唯一 PC 端 CDC 写入者；不要与串口监视器争用同一运行时 COM 口。
Agent 每帧为 UTF-8 JSON 加 LF：

```json
{"cpu":20.0,"mem":50.0,"cpu_temp":null,"status":"coding"}
```

cpu/mem 单位为百分比，范围 0～100；cpu_temp 为摄氏度或 null；status 为 normal/coding/browser/media。
固件限制单帧 512 字节；非法 JSON、无效类型/范围、未知 status 或尾部垃圾拒绝；缺字段保留上次有效字段。
PC 超时 5 秒转 Offline。仅 CPU/RAM 等字段的消息不能触发演示环境的首次自动解锁；status 必须实际有效且经过约 800ms 去抖。

| 页 | 实际动作 |
|---|---|
| Control | Ctrl+C、Ctrl+V、Alt+Tab、Win+D，ID 0～3 固定 |
| Normal / Quick Actions | Win+E，以及通用窗口操作/工具入口 |
| Coding | Ctrl+Shift+P、Ctrl+P、Ctrl+反引号、Ctrl+/；encoder_preview 另有 Ctrl+D（ID21） |
| Browser | Ctrl+T、Ctrl+W、Ctrl+Shift+T、Ctrl+L、Ctrl+R、Alt+Left |
| Media | 播放暂停、上一首、下一首、音量增减、静音 |

键盘使用 HID report ID1、8 字节；Consumer 使用 ID4、2 字节。按下后保持 30ms，再发送全释放报告。
发送失败后继续尝试释放；未 ready 的按下不重放。发送结果不是目标应用执行 ACK。
旋转积压先移动焦点再确认；长按不执行宏。SDL 模拟器仅打印意图。
上下文分类是前台窗口/进程规则，不是 AI，也不证明后台音乐正在播放。当前 Agent 没有歌曲、歌词或播放进度数据。
