@echo off
chcp 65001 >nul
cd /d "%~dp0"
echo 声境 Connect 本地页面
echo 启动后请在浏览器打开 http://127.0.0.1:4173
echo 保持此窗口开启。按 Ctrl+C 停止服务。
python serve.py --open
pause
