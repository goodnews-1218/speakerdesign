# Windows Connect classroom project

## 本地语音与画面对话页面

新增前端位于 `frontend/`。双击 `frontend/start.cmd` 可启动本地服务并打开页面，
也可运行 `python frontend/serve.py` 后访问 `http://127.0.0.1:4173`。
页面展示摄像头画面、语音识别文字和模型回答，提供演示模式、历史记录及导出。
本地服务支持按语音或文字指令从音箱拍照、调用 DeepSeek 并播报结果。
填写 API Key 和连接设备的步骤见 [前端说明](frontend/README.md#用语音描述眼前画面)。

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
