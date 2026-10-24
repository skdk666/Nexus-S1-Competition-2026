# 公开源码副本说明

交付日：2026-10-24。来源为维护者当前工作树；基线提交 82d40ed23908707914e4df9d4252820db7cbcbff，但源码包含其后的未提交修改。
旧远端实时权限不足，不能证明它已包含这些代码；也不能从 404 推断其可见性。

保留：当前全部固件源/头文件、LVGL 配置、统一 Agent 与启动/卸载脚本、相关测试/诊断、共享 UI 模拟器。
排除：原 .git、.pio、src/ui.o 等产物、旧比赛笔记、审计历史、日志/备份/ZIP、旧 data.py/text_context.py、旧 Web 和 Chart.js、旧启动包装脚本及无运行内容的根目录占位工程。
旧 Web 不在当前 USB 演示链路中；SPIFFS 挂载/枚举代码仍完整保留，不依赖随包网页。

仅对副本做的发布适配：

- AP 密码继续使用被忽略的 local_secrets.h；未复制本机配置或现有密码。
- 旧 MQTT 地址移到本地 NEXUS_MQTT_HOST/NEXUS_MQTT_PORT，模板为无服务的 broker.invalid；本地 USB 核心行为不变。
- PlatformIO/库固定为本机验证版本；清除禁用 OTA 上传示例中的旧地址和未验证帧率宣传，不启用 OTA。
- 安装器删除一条个人 Anaconda 绝对路径，保留显式 -Python、用户目录和 PATH 回退。
- 测试将真实 USB 序列号改为合成 fixture；候选路径断言改为用户目录，保留原断言意图。
- 主机回归编译器改用 NEXUS_HOST_CXX_DIR/PATH，断言不变。
- 新写 README、接线/协议/许可证/硬件缺项等文档，代替失真的旧宣传内容。

不改变编码器/GPIO、显示/SPI 初始化、LVGL tick、触摸采样、HID 映射/释放或场景导航业务。
Git 记录显示的开发作者账号在 AUTHORS.md 保留；不复制含历史凭据的 Git 对象。
SOURCE_MANIFEST.csv 逐文件记录 SHA-256、来源及是否为发布适配；它不对自身哈希。

为保证 Git 上传和跨平台检出后字节哈希一致，.gitattributes 使用 -text 禁用自动换行转换；保留源码现有行尾，不改变业务代码。
