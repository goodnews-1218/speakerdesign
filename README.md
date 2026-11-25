# Windows Connect classroom project

## 本地语音与画面对话页面

新增前端位于 `frontend/`。双击 `frontend/start.cmd` 可启动本地服务并打开页面，
也可运行 `python frontend/serve.py` 后访问 `http://127.0.0.1:4173`。
页面展示摄像头画面、语音识别文字和模型回答，提供演示模式、历史记录及导出。
真实音箱采集和大模型需要配套电脑端服务；接入方式见 [前端说明](frontend/README.md)。

## 原课程项目说明

课程包 v1.1。真实设备目标为 Windows x64 + Visual Studio 2022 C++ 工具链 + CMake 3.24+。
Trae 编辑代码；CMake 和 MSVC 编译。基础实验不需要安装 Qt，不打开摄像头，不接入云端转写。

## 构建和运行

在本目录的 PowerShell 终端执行：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\build.ps1
.\build-student\Release\connect_lab.exe --mode mic --seconds 10 --output recordings
```

Bypass 仅用于运行本包可审阅的脚本，不改变系统永久执行策略。也可使用教材里的 cmake 命令。
EXE 旁必须保留两个 DLL 和描述 JSON；构建步骤会复制。不要单独拷贝 EXE。
程序等到 CONNECTED 后查询功能模式，检查开始录音两条响应，接收音频，再检查停止响应。
只在全部条件通过时显示 LAB_RESULT=PASS；退出码 1 表示未完整通过，2 表示参数/初始化问题。

每轮在 recordings/session-时间戳下生成 mic.wav 和/或 spk.wav，仅收到数据的来源才生成文件。
WAV 为 16000 Hz、单声道、16-bit。双击 mic.wav 在本地播放器回听；请使用中性测试句。
程序会在回调中复制借用数据，再由有界队列和后台线程写文件。DROPPED/REJECTED 非零不可当完整录音。
正常结束或 Ctrl+C 都尝试停止设备、关闭摄像头、close/destroy，最后排空写入队列并完成 WAV 头。
强制结束进程或拔电不保证写完 WAV；课堂不以任务管理器结束进程作为正常停止步骤。

若一直重连，请教师从设备管理器的硬件 ID 确认本批设备的 VID/PID，再同时指定，例如：

```powershell
.\build-student\Release\connect_lab.exe --mode mic --seconds 10 --vid 0x2207 --pid 0x0017
```

示例值是 Windows SDK 默认值，不能保证适用于全部型号。不要照抄 macOS SDK 的设备 ID 或头文件。
一台电脑只接一台目标 Connect。设备未响应、无 PCM、只有排队成功或只有一条响应，都不能记为通过。

## 可选语音功能

使用 Windows x64 的 Python 3.12–3.14 下载 Kokoro 播报模型、SenseVoice 识别模型及
Sherpa-ONNX 运行时。脚本从官方发布页下载固定版本并校验 SHA-256，写入被 Git 忽略的
`build-student`。在项目目录执行：

```powershell
python -B .\scripts\download_voice_models.py
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\build.ps1
```

构建需要终端可以找到 `cmake` 和 `ctest`。如果以前配置过不同的模型路径，可把
`CONNECT_VOICE_MODEL_DIR` 环境变量设为下载后的 SenseVoice 文件夹。

语音对话需要在 `scripts/deepseek.local.toml` 中填入 `api_key = "你的 Key"`，或在当前终端
设置 `DEEPSEEK_API_KEY`。配置文件已被 Git 忽略。把 Connect 设为 Windows 默认声音输出，
用安装依赖时的同一个 Python 运行：

```powershell
python -B -X utf8 .\scripts\voice_chat.py
```

录像需要 FFmpeg 在 `PATH` 中。录像时按 `S` 可以保存截图，启用语音抓拍后也可说「拍张照片」：

```powershell
.\build-student\Release\connect_lab.exe --video --voice-snapshot --mode mic --seconds 30
```

### 长录像与精彩片段

```powershell
.\build-student\Release\connect_lab.exe --video --continuous --mode mic --voice-snapshot
```

在录像终端按 `H`，或说「保存精彩片段」「保存这段视频」后稍停，会标记前后各 20 秒。
片段准备好后，终端显示 `CLIP_SAVED=highlight-1.mp4`。开头或提前结束时，保存实际录到的部分。
保留 `S` 截图。若要用音箱实体键，先看按键日志中的 `DEVICE_KEY` 和 `ACTION`，
确认选中的键会上报单击（`ACTION=2`），并确认它的原生动作适合录像，再用
`--highlight-button N` 绑定。

电脑端每 120 秒写出一个带音频的 `segments/segment-N.mp4`，切段时音箱继续采集。
按 `Ctrl+C` 停止并等待本次会话输出完整的 `video.mp4`。可用 `--seconds 180` 替代
`--continuous` 定时结束；`--segment-seconds N` 可调整分段长度。视频和精彩片段在
`both` 模式使用麦克风轨，`spk` 模式使用扬声器轨。

## 智能戒指心率与精彩片段

启动 `BioScopeAPI.exe`，通过其 `/connect` 接口连接戒指 `BCL603M5`（`device_type = 'ppg'`），确认状态为 `streaming`。Connect 在后台每秒从本机 BioScope 读取 `GREEN` PPG，使用最近约 5–7 秒的波形估算心率。终端显示 `RING_BPM`，视频左下角显示随估算心率跳动的心形和 BPM 数字。

- 如果脉搏峰间隔不够规律，程序用自相关检查整窗和最近约 4 秒的波形。如果两段波形仍支持近期已验证的脉搏周期，还可以在压低尖峰影响后进行补估。新补估值显示为心率数字，并在终端打印 `RING_RECOVERED_BPM`。
- 样本仍在更新但暂时无法估计新心率时，视频先用 `HOLD` 标记显示上一次读数约 3 秒。超过保持期、数据停更或断开后，显示 `LAST` 和距上次估算的秒数。首次得到读数前，或戒指采集会话切换时，显示 `WAIT`，直到新会话产生读数。基线和自动片段继续使用通过峰值检查的新估算。

只看终端读数（无需连接音箱，Ctrl+C 停止）：

```powershell
.\build-student\Release\connect_lab.exe --ring-monitor
```

录音录像并显示心率：

```powershell
.\build-student\Release\connect_lab.exe --video --continuous --mode mic --voice-snapshot --ring
```

用固定个人基线开启自动片段：

```powershell
.\build-student\Release\connect_lab.exe --video --continuous --mode mic --voice-snapshot --ring --ring-auto-highlight --ring-baseline 90
```

- `--ring-baseline auto` 为自动片段模式的默认值。开始时静坐，等 `RING_BASELINE` 打印数值。程序用连续 30 秒稳定读数的中位数作为本次戒指会话基线。可用 `--ring-calibration-seconds` 修改校准时长。
- `--ring-baseline 90` 将基线设为 90 BPM，默认达到 108 BPM 或降到 72 BPM 开始计时。也可用 `--ring-baseline 70:80` 指定区间，此例在达到 96 BPM 或降到 56 BPM 时开始计时。
- 偏离达到基线边界的 20% 并持续至少 6 秒，终端打印 `RING_HIGHLIGHT_TRIGGERED`。可用 `--ring-change-percent` 和 `--ring-hold-seconds` 调整。连续 3 秒回落到恢复阈值内后结束事件。恢复阈值使用配置偏离量的 80%，减少边缘抖动。
- 自动片段名为 `highlight-ring-*.mp4`，包含事件起点前约 20 秒、整个事件及结束后约 20 秒。持续超过 60 秒的事件分段保存，停止录像时会导出已确认事件的已有部分。检测基于滚动估计，片段边界对应估计心率变化的时刻。
- `H`、设备按键和语音精彩片段仍然可用。整段视频、分段视频和精彩片段都使用叠加后的画面。

带 `DEMO` 水印的演示输入（无需戒指，正式版仍使用真实摄像头和音频）：

```powershell
.\build-student\Release\connect_lab.exe --video --seconds 40 --mode mic --ring-demo --ring-auto-highlight --ring-baseline 75
```

当前功能用于记录脉搏变化与对应视频。BPM 取自光学 PPG，运动干扰会影响读数。医疗监护与诊断需要经过相应验证的设备。

## 学生任务

src/LessonTask.h 的 recordingSummary 是可运行占位实现。只修改它，输出
`mic_bytes=32000; audio_seconds=1.00` 这样的摘要，音频时长按 bytes/32000.0 计算。

```powershell
ctest --test-dir build-student -C Release -R lesson_summary --output-on-failure
```

首次应失败；实现后先重新构建，再运行测试。不要修改测试预期。摘要只计算 MIC 数据时长，
不是墙上时间，不是识别文本，也不代表 SPK 通道验收。

教师参考代码位于 answers/LessonTask.h，启用它需重新配置独立目录：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\build.ps1 -TeacherAnswer
ctest --test-dir build-teacher -C Release --output-on-failure
```

## 选修和证据

--mode spk / both 供教师课前验证后选用。扬声器来源可能需要电脑实际向 Connect 播放测试音频；
无 SPK 数据时不能用 MIC 文件代替。--mode both 要求两个来源都有数据。
基础项目不自动把音频送往 ASR；耳机 Android SDK 的 final 回调不能直接用于这套 Windows SDK。

tests/fake_sdk.cpp 仅用于主流程自动检查。-DCONNECT_LAB_SIMULATOR=ON 生成
connect_lab_simulated，控制台明确显示 SIMULATED；其音频为测试字节，不能作为硬件运行证据。
可移植自检与 Windows 编译、链接、DLL 加载、真实 USB/音频验收分开记录，见课程包的验证状态。

vendor 来源为原交付 Release.zip，公共 ABI 4.0.0，保持原样；课程包版本与 SDK 版本分别管理。
预编译 Windows EXE 尚需由学校或提供方在 Windows 上构建、验证后分发；本包没有把模拟程序冒充体验 EXE。
