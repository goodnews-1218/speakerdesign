# 声境 Connect 本地前端

不需要安装前端依赖。实际操作音箱前，在项目根目录编译 Windows x64 Release：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\build.ps1
```

构建会生成 `build-student/Release/connect_lab.exe` 和 `connect_frame.exe`。程序旁需要保留构建复制的厂商 DLL 和描述 JSON。双击 `start.cmd` 会启动本地服务并打开默认浏览器，也可以在仓库根目录执行：

```powershell
python frontend/serve.py
```

在 Edge 或 Chrome 中打开 <http://127.0.0.1:4173>。停止时在启动窗口按 Ctrl+C。端口被占用时可运行 `python frontend/serve.py --port 4174`。

## 用语音描述眼前画面

1. 打开 `frontend/deepseek.env`，将 DeepSeek 开放平台创建的 **API Key** 填在 `DEEPSEEK_API_KEY=` 的等号后。如果这个文件不存在，复制 `deepseek.env.example` 到同目录并改名为 `deepseek.env`。文件已加入 Git 忽略规则。也支持通过本机环境变量设置同名项。
2. 用 USB 连接 Connect，打开镜头盖。在 Windows「设置 → 系统 → 声音 → 输出」选择该音箱，并先用系统测试音确认能播出。
3. 打开页面的「连接设置」，选择「音箱服务」，电脑端服务地址留空。点「开始语音输入」。听到屏幕阅读器提示可以说话后，说「描述一下你看到的画面」，再点「结束语音输入」。也可以直接等约 12 秒自动结束。
4. 识别到指令后，电脑端会抓取一张新的照片，把照片和指令发送给 DeepSeek。回答在页面上显示，电脑会通过 Windows 默认输出设备播报。

也可直接在页面输入「描述一下你看到的画面」再发送，或点「描述当前画面」。没有连接音箱时，可切换到「电脑麦克风」模式，手动开启电脑摄像头预览，再用文字或按钮测试拍照描述。拍照会发送当前照片，录音转写会发送这轮音频。用于了解静态画面，过街和避障需要其他可靠方式。

如果设备用不同的 USB 硬件 ID，可通过 `python frontend/serve.py --vid 0x2207 --pid 0x0017` 同时覆盖两个值。示例为 SDK 默认值，实际以设备管理器识别的设备为准。若系统输出列表中找不到这台音箱，需要厂商确认 Windows 播放驱动或提供音频播放接口。

## 在网页查看戒指预警

重编译后，双击 `start.cmd`，进入页面的「戒指预警」。启动 BioScope 并确认戒指的数据流是 `streaming`，接上 Connect 并选为 Windows 默认声音输出。录像需要 FFmpeg 在 `PATH`，真实预警分析使用 `frontend/deepseek.env` 中的 API Key。

在「心率基线」选择自动校准或手动输入（例如 `75` 或 `65:85`），然后点「开始监测」。网页连续预览摄像头画面，画面上叠加当前心率，保存的录像包含声音。

默认规则是在有效心率达到基线区间边界的 20% 偏离、持续满 6 秒时立即确认一次心率预警并排入抽帧分析。回落到恢复阈值内连续 3 秒用于结束当前心率事件。DeepSeek 会分析是否出现跌倒、坐姿突然瘫软，或预警确认后同一人至少 6 秒持续坐着且未见明显动作。确认后会保留一批后续画面，即使心率先恢复，也可以检查后续姿势。

出现任一可疑画面结果后，音箱询问「你摔倒了吗？如果你没事，请说，我没事。」播完后采集约 8 秒音箱麦克风语音用于识别。收到明确的「我没事」会取消同一次事件的警报声，但保留记录。未收到明确确认或语音功能故障时播放警报，请人工查看现场。预警画面与这段回应可发送至 DeepSeek。

心率预警后，画面出现疑似跌倒、坐姿突然瘫软或持续坐姿不动中的任一种，并且语音窗口未收到回应时，事件记录为高优先级。语音功能故障有单独状态，仍会响铃。

「预警进度」显示当前处理状态。「预警记录」按事件展示心率异常前后的画面和观察。缺少异常起点的旧记录按预警确认时刻划分。记录保存在 `recordings/session-*/fall-alerts/`，重启或开始新监测后仍可查看，较早的记录可用「加载更早的预警」展开。

「试听警报」用 Windows 默认声音输出播放测试音，可以确认声响是否来自连接的音箱。

结束时点「停止监测」，等待已触发预警的分析和录像保存。停止后若才收到疑似跌倒结果，页面会保留结果并标明停止后未自动响铃。刷新浏览器或切换到对话页面时，监测由本机服务继续进行。

要用模拟心率测试完整流程，勾选「模拟心率」和「测试完整预警流程」。仍需连接 Connect 摄像头和麦克风、配置 Windows 默认输出、准备 FFmpeg 和 API Key。可分别测试坐稳后回答「我没事」与保持安静，观察语音确认和响铃两个分支。命令行可使用 `python frontend/ring_alert.py --ring-demo --analyze-demo --ring-baseline 75`。

只勾选「模拟心率」会把抽帧和录像保存在本机。详细文件位置和命令行用法见 [戒指预警说明](../README.md#心率预警时自动检查疑似跌倒)。

## 已实现

- 设备画面、语音输入、可编辑识别文字、模型回答同时展示；适配桌面和手机。
- 演示模式：普通对话使用明确标记的示例。用户仍可主动发起真实的画面描述。
- 电脑 / USB 摄像头预览、设备选择、全屏、截图；打开时才申请摄像头权限。
- 音箱画面：原生 SDK 单次拍照会显示本轮照片。持续预览可以连接另有提供的 MJPEG 或浏览器视频地址。
- 浏览器语音识别：使用默认麦克风、显示中间识别文字。浏览器支持和网络环境会影响可用性，识别可能使用浏览器提供商的在线服务。
- 导入音频、回听；切换到真实模式并配置服务后，可提交音频识别。
- 通过电脑端服务发送文字并显示真实模型回答；错误、超时与取消处理。
- 浏览器朗读（使用系统默认声音输出）、复制、导出、最近对话（最多 30 条，本机浏览器存储）。

## 输入、拍照和播报接口

默认使用与页面同源的电脑端服务。API Key 只由电脑端读取。文字和语音入口都能触发一次拍照。原来的 C++ `--voice-snapshot` 仍用于本地识别拍照短指令。

| 环节 | 本地实现 |
|---|---|
| 输入 | 文字直接提交。音箱语音用 `connect_lab --mode mic`，设备确认后录制 16 kHz 单声道 WAV，再调用 DeepSeek [语音识别](https://api-docs.deepseek.com/zh-cn/guides/asr/)。 |
| 拍照 | `connect_frame.exe` 通过厂商摄像头回调取得一张新帧，用 Windows WIC 转成 JPEG。需要时也可手动运行 `.\build-student\Release\connect_frame.exe --output frame.jpg`。 |
| 看图 | 本机服务将 JPEG 和指令传给 `deepseek-flash` 的对话接口。 [图像理解](https://api-docs.deepseek.com/zh-cn/guides/vision/) |
| 播报 | DeepSeek [语音合成](https://api-docs.deepseek.com/zh-cn/guides/tts/)返回音频。Windows 用默认输出播放，页面提供重播。厂商 `spk` 接口用于录制扬声器侧音频。 |

### 拍照描述与播放

`POST /api/describe` 接受 `{"text":"描述一下你看到的画面"}`，服务从音箱新拍一张。电脑摄像头模式可在同一请求中加入 `image`，值为浏览器从当前画面获取的 JPEG data URL。响应包含 `answer`、`model`、`image_url`、`audio_url` 和 `played`。图片和播报音频使用有效期至多五分钟的本机地址。语音合成失败时仍返回描述文字，浏览器可尝试系统朗读。

`POST /api/playback/stop` 接受空 JSON 对象，用于停止本机正在播放的描述。

### 状态

`GET /api/status`

```json
{"device":{"connected":false,"name":"Connect"},"model":{"connected":true,"name":"your-model"}}
```

两个 `connected` 为布尔值。音箱状态表示最近一次采集结果。首次操作前 `device.checked` 为 `false`。填写 Key 后 `model.configured` 为 `true`，实际收到 DeepSeek 响应后才显示连接成功。

### 音箱录音与识别

`POST /api/recording/start`，JSON 请求：

```json
{"session_id":"frontend-generated-id"}
```

设备确认开始后返回 `{"recording":true}`。

`POST /api/recording/stop`，同样传 `session_id`，完成识别后返回：

```json
{"recording":false,"transcript":"识别出的完整文字"}
```

取消或离开页面时，前端提交相同的 `session_id` 并设置 `discard: true`。录音最多持续约 12 秒。

`POST /api/transcribe` 使用 `multipart/form-data`，字段 `audio` 为原始音频文件。DeepSeek 支持不超过 8 MiB、最长 120 秒的 16 位 PCM WAV 或 Ogg Opus。返回 `{"text":"识别文字"}`。

### 模型回答

`POST /api/chat`

```json
{"text":"用户的问题","session_id":"frontend-generated-id"}
```

返回 `{"answer":"模型回答","model":"模型名称"}`。本机服务逐条处理请求。

### 外接服务的实时事件（可选）

`GET /api/events` 使用 `text/event-stream`，发送普通 SSE message，每条 `data` 为 JSON，例如：

```text
data: {"type":"transcript","session_id":"frontend-generated-id","text":"正在识别的文字","final":false}

data: {"type":"status","device":{"connected":true,"name":"Connect"},"model":{"connected":true,"name":"your-model"}}

data: {"type":"error","message":"设备已断开"}

```

无 SSE 时，开始 / 停止录音及识别响应仍可完成整段交互。SSE 状态中断时页面保守显示未连接，等待状态重新确认。

### 画面

连续视频使用外接服务提供 `GET /api/video.mjpeg`，类型 `multipart/x-mixed-replace; boundary=frame`，逐帧发送 JPEG。前端通过独立的“音箱画面地址”加载，也支持浏览器可直接播放的 `.mp4` / `.webm` URL。单次拍照描述由本机服务直接连接音箱，无需填写这个地址。

截图对跨域图像受浏览器限制；同源服务或电脑 / USB 摄像头可以保存。若要跨域截图，后续应配置正确的 CORS 与媒体加载方式，或提供服务端截图接口。

### 本地服务配置

- 内置服务仅监听 `127.0.0.1`，页面和接口共用端口。
- 若另有不同端口的外接服务，由外接服务配置页面来源和跨域访问。
- API 错误使用非 2xx HTTP 状态，并可返回 `{"error":"便于用户理解的原因"}`。
- 画面描述的前端请求超时为 100 秒。

## 文件

- `dist/index.html`：页面与对话框。
- `dist/styles.css`：响应式样式。
- `dist/app.js`：交互、媒体预览、演示及服务适配。
- `serve.py` / `start.cmd`：启动本地页面和设备服务。
- `recording.py`、`deepseek.py`：音箱录音桥接和 DeepSeek 接口。
