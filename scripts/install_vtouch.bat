@echo off
setlocal EnableExtensions
rem vtouch Windows + ADB 一键更新/安装/启动
rem 用法：install_vtouch.bat [宽] [高]

set "ADB=%~dp0platform-tools\adb.exe"
if not exist "%ADB%" set "ADB=adb"
set "WIDTH=%~1"
set "HEIGHT=%~2"
if "%WIDTH%"=="" set "WIDTH=1440"
if "%HEIGHT%"=="" set "HEIGHT=3168"

set "DAEMON=%~dp0..\build\vtouchd"
set "CLIENT=%~dp0..\build\vtouchctl"
set "WS=%~dp0..\build\vtouchws"

if not exist "%DAEMON%" ( echo [ERROR] 找不到 build\vtouchd & exit /b 1 )
if not exist "%CLIENT%" ( echo [ERROR] 找不到 build\vtouchctl & exit /b 1 )
if not exist "%WS%" ( echo [ERROR] 找不到 build\vtouchws & exit /b 1 )

"%ADB%" start-server >nul
"%ADB%" get-state >nul 2>nul || (echo [ERROR] 未检测到设备& "%ADB%" devices& exit /b 1)

for /f "delims=" %%i in ('"%ADB%" shell su -c id') do set "ROOTID=%%i"
echo %ROOTID% | findstr /C:"uid=0" >nul || (echo [ERROR] adb shell 没有 root& exit /b 1)

echo [1/6] 停止旧实例并清理运行环境...
"%ADB%" shell su -c "if [ -x /data/local/tmp/vtouchctl ]; then /data/local/tmp/vtouchctl reset >/dev/null 2>&1 || true; fi; killall vtouchws >/dev/null 2>&1 || true; killall vtouchd >/dev/null 2>&1 || true; sleep 1; rm -f /data/local/tmp/vtouchd /data/local/tmp/vtouchctl /data/local/tmp/vtouchws /data/local/tmp/vtouch.sock /data/local/tmp/vtouchd.log /data/local/tmp/vtouchws.log"

for %%F in (vtouchd vtouchctl vtouchws) do "%ADB%" shell rm -f /sdcard/%%F.update
"%ADB%" push "%DAEMON%" /sdcard/vtouchd.update >nul || exit /b 1
"%ADB%" push "%CLIENT%" /sdcard/vtouchctl.update >nul || exit /b 1
"%ADB%" push "%WS%" /sdcard/vtouchws.update >nul || exit /b 1

echo [2/6] 安装本次构建文件...
"%ADB%" shell su -c "cp /sdcard/vtouchd.update /data/local/tmp/vtouchd; cp /sdcard/vtouchctl.update /data/local/tmp/vtouchctl; cp /sdcard/vtouchws.update /data/local/tmp/vtouchws; chmod 755 /data/local/tmp/vtouchd /data/local/tmp/vtouchctl /data/local/tmp/vtouchws"

for /f %%i in ('"%ADB%" shell su -c "ps -A -o NAME ^| grep -w vtouchd ^| wc -l"') do if not "%%i"=="0" (echo [ERROR] 旧 vtouchd 未清理& exit /b 1)
for /f %%i in ('"%ADB%" shell su -c "ps -A -o NAME ^| grep -w vtouchws ^| wc -l"') do if not "%%i"=="0" (echo [ERROR] 旧 vtouchws 未清理& exit /b 1)

echo [3/6] 启动 vtouchd...
"%ADB%" shell su -c "nohup /data/local/tmp/vtouchd -x %WIDTH% -y %HEIGHT% >/data/local/tmp/vtouchd.log 2>&1 </dev/null &"
timeout /t 2 /nobreak >nul
"%ADB%" shell su -c "/data/local/tmp/vtouchctl ping" || (echo [ERROR] vtouchd 未就绪& "%ADB%" shell su -c "cat /data/local/tmp/vtouchd.log"& exit /b 1)

for /f %%i in ('"%ADB%" shell su -c "ps -A -o NAME ^| grep -w vtouchd ^| wc -l"') do if not "%%i"=="1" (echo [ERROR] vtouchd 不是单实例& exit /b 1)

echo [4/6] 启动 WebSocket...
"%ADB%" shell su -c "nohup /data/local/tmp/vtouchws >/data/local/tmp/vtouchws.log 2>&1 </dev/null &"
timeout /t 1 /nobreak >nul
for /f %%i in ('"%ADB%" shell su -c "ps -A -o NAME ^| grep -w vtouchws ^| wc -l"') do if not "%%i"=="1" (echo [ERROR] vtouchws 不是单实例& exit /b 1)

"%ADB%" shell settings put system show_touches 0 >nul 2>nul
"%ADB%" shell settings put system pointer_location 0 >nul 2>nul

echo [5/6] 验证服务...
"%ADB%" shell su -c "/data/local/tmp/vtouchctl ping"
"%ADB%" shell su -c "ss -ltn | grep 27183"

echo [6/6] 完成
 echo [OK] 最新程序已安装并启动，且 vtouchd/vtouchws 均为单实例。
echo resolution=%WIDTH%x%HEIGHT%
echo websocket=ws://127.0.0.1:27183
endlocal
