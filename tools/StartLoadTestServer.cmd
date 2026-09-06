@echo off
cd /d "%~dp0"
echo Local load-test server: 127.0.0.1:17891, maximum 65536 sessions.
echo Stop this server with Ctrl+C. Keep this window open during the test.
SummitServer.exe --address 127.0.0.1 --port 17891 --max-sessions 65536 --metrics-interval-ms 1000
pause
