# 声境 Connect 本地前端

不需要安装前端依赖。双击 `start.cmd` 会启动服务并打开默认浏览器，或在仓库根目录执行：

```powershell
python frontend/serve.py
```

在 Edge 或 Chrome 中打开 <http://127.0.0.1:4173>。停止时在启动窗口按 Ctrl+C。端口被占用时可运行 `python frontend/serve.py --port 4174`。

## 已实现

- 设备画面、语音输入、可编辑识别文字、模型回答同时展示；适配桌面和手机。
- 演示模式：模拟识别和逐字回答，所有示例都明确标记，不调用设备或模型。
- 电脑 / USB 摄像头预览、设备选择、全屏、截图；打开时才申请摄像头权限。
- 音箱视频服务：显示 MJPEG / 图片流或浏览器支持的视频 URL；也能回放本地视频。
- 浏览器语音识别：使用默认麦克风、显示中间识别文字。浏览器支持和网络环境会影响可用性，识别可能使用浏览器提供商的在线服务。
- 导入音频、回听；切换到真实模式并配置服务后，可提交音频识别。
- 通过电脑端服务发送文字并显示真实模型回答；错误、超时与取消处理。
- 浏览器朗读（使用系统默认声音输出）、复制、导出、最近对话（最多 30 条，本机浏览器存储）。

## 当前边界

这次新增的是独立前端，不修改现有 C++ 采集程序。仓库中的 C++ 程序仍没有 HTTP / SSE 服务；`VoiceSnapshot` 的识别目前只处理拍照命令，不能直接提供完整对话转写。模型和音箱接口需按下列契约另行实现。

画面预览不自动上传给大模型。浏览器摄像头只有在 Windows 将设备暴露为可用视频输入时才能选到；否则使用电脑端程序将 SDK 图像帧转换为 MJPEG。不得用演示成功代替硬件验收。

朗读通过电脑默认输出播放。要从音箱发声，需要在 Windows 声音设置中选择该音箱。网页没有通过厂商 SDK 直接发送音频。

## 电脑端服务契约

连接设置填写服务根地址，例如 `http://127.0.0.1:8765`。下列是待实现的接口契约，不是已经运行的后端。API 密钥只放服务端，不写入前端文件或浏览器存储。

### 状态

`GET /api/status`

```json
{"device":{"connected":false,"name":"Connect"},"model":{"connected":true,"name":"your-model"}}
```

两个 `connected` 必须为布尔值，只有真实检测成功才返回 `true`。

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

取消或离开页面时会尽力发送 `{"discard":true}`。后端仍应有录音超时保护，不依赖网页一定能发送停止请求。

`POST /api/transcribe` 使用 `multipart/form-data`，字段 `audio` 为原始音频文件，`session_id` 为会话标识。前端限制文件 30 MB，后端需验证真实格式和体积；浏览器支持的音频不等于识别服务支持的音频。返回 `{"text":"识别文字"}`。

### 模型回答

`POST /api/chat`

```json
{"text":"用户的问题","session_id":"frontend-generated-id"}
```

返回 `{"answer":"模型回答","model":"模型名称"}`。当前使用完整 JSON 回答；需要多轮上下文时由服务端按 `session_id` 管理。

### 实时事件（可选）

`GET /api/events` 使用 `text/event-stream`，发送普通 SSE message，每条 `data` 为 JSON，例如：

```text
data: {"type":"transcript","session_id":"frontend-generated-id","text":"正在识别的文字","final":false}

data: {"type":"status","device":{"connected":true,"name":"Connect"},"model":{"connected":true,"name":"your-model"}}

data: {"type":"error","message":"设备已断开"}

```

无 SSE 时，开始 / 停止录音及识别响应仍可完成整段交互。SSE 状态中断时页面保守显示未连接，等待状态重新确认。

### 画面

建议提供 `GET /api/video.mjpeg`，类型 `multipart/x-mixed-replace; boundary=frame`，逐帧发送 JPEG。前端通过独立的“音箱画面地址”加载；也支持浏览器可直接播放的 `.mp4` / `.webm` URL。RTSP 和原始 BGR24 不能直接填进网页。

截图对跨域图像受浏览器限制；同源服务或电脑 / USB 摄像头可以保存。若要跨域截图，后续应配置正确的 CORS 与媒体加载方式，或提供服务端截图接口。

### 本地服务配置

- 优先仅监听 `127.0.0.1`。
- 如果服务与网页端口不同，允许网页来源 `http://127.0.0.1:4173`，处理 `OPTIONS`，支持 `GET, POST` 与 `Content-Type`。
- API 错误使用非 2xx HTTP 状态，并可返回 `{"error":"便于用户理解的原因"}`。
- 前端一般请求超时 25 秒，模型回答超时 90 秒。

## 文件

- `dist/index.html`：页面与对话框。
- `dist/styles.css`：响应式样式。
- `dist/app.js`：交互、媒体预览、演示及服务适配。
- `serve.py` / `start.cmd`：仅提供静态文件的本地启动工具。
