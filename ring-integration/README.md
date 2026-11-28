# 智能戒指与音箱联动

## 双击戒指描述画面

1. 在 Windows「设置 → 蓝牙和设备」配对戒指。在记事本里双击戒指触摸区，如果出现一次换行，说明 Windows 收到了戒指按键。
2. 用 USB 连接音箱并打开镜头盖。在 Windows「设置 → 系统 → 声音 → 输出」中选择音箱。
3. 在 `frontend/deepseek.env` 配置本机的 DeepSeek API Key，然后双击 `frontend/start.cmd` 启动 Connect Studio。
4. 在「对话工作台」点「戒指双击描述」，保持声境页面在前台。双击戒指触摸区后，前端会展示音箱画面和模型回答，并播报回答。也可以按一次键盘 Enter 模拟双击。
5. 单独调试时，可从「戒指预警」打开「戒指联动测试」。点一次「开始测试」，确认听到提示音，再双击戒指触摸区。也可点击「描述当前画面」检查同一功能。

戒指固件把触摸双击发送成一个蓝牙键盘 Enter。触发双击描述时保持工作台或联动测试页在前台。联动测试页也支持在 4 秒内做两次向下动作，播报「我摔倒了」，可用两次 Page Down 调试。这个求救手势和主页面里的心率预警、画面分析与语音确认可以分别测试。

## 即时测量

- Windows 测量。运行 `ring-integration/build.cmd` 编译 `RingBridge.exe`，双击打开程序并保留控制台窗口。回到联动页，点击「使用 Windows 已配对的戒指测量」，保持手静止，等待约 30 秒读取心率、血氧及戒指测得温度。之前使用的地址 `http://127.0.0.1:8765/` 会跳转到默认端口的 Connect Studio 联动页。
- 网页蓝牙测量。Windows 上的 Edge 或 Chrome 可尝试「连接戒指蓝牙数据」「重连已授权戒指」或「显示全部附近设备」。选中戒指后点击「开始测量」。如果平板上的 nRF Connect 仍连着戒指，请先断开。浏览器授权按网址和浏览器个人资料保存。
- 两种即时读数使用戒指的自定义 BLE 测量服务。Windows 桥接默认识别 `BCL6039F5A`，换兼容戒指时可在 `RingBridge.cs` 修改设备名再编译。
- 主页面用于心率预警和录像的实时监护继续使用 BioScope 的连续 PPG 数据。即时测量页面用于查看一次读数，实时监护由 BioScope 独立提供数据。

## 文件和接口

| 文件 | 用途 |
| --- | --- |
| 戒指音箱联动测试.html | 手势接收、画面描述与播报、即时 BLE 测量界面。 |
| RingBridge.cs | 通过 Windows 已配对设备访问戒指，提供仅限本机的 `POST /measure`。 |
| build.cmd | 使用 Windows SDK 的 UnionMetadata 和 .NET Framework 4.8 或 4.7.2 Developer Pack 重新编译。更新之前请关闭旧版桥接程序。 |

Connect Studio 在 `/ring-integration/` 提供联动页。双击调用现有的 `POST /api/describe`，通过音箱画面获取模型回答。Windows 测量按钮调用同一服务的 `POST /api/ring/measure`，服务再连接本机桥接。只修改 HTML 时刷新浏览器即可。
