@echo off
rem Конвертация фильма для CamBrowser (ESP32-P4, SW H.264).
rem По умолчанию smooth: 320x180@20 Baseline (плавно на P4).
rem Третий аргумент: smooth | max
rem Нужен ffmpeg в PATH.

if "%~1"=="" (
    echo usage: %~nx0 input.mkv [output.mp4] [smooth^|max]
    exit /b 1
)
if "%~2"=="" (set OUT=%~dpn1_cam.mp4) else (set OUT=%~2)
set PROF=%~3
if "%PROF%"=="" set PROF=smooth

if /I "%PROF%"=="max" (
  set W=480& set H=270& set FPS=20& set BV=500k& set MAXR=800k& set BUF=1200k& set LVL=3.0
) else (
  set W=320& set H=180& set FPS=20& set BV=250k& set MAXR=400k& set BUF=600k& set LVL=2.1
)

echo profil: %W%x%H% @ %FPS% Baseline
ffmpeg -y -i %1 -map 0:v:0 -map 0:a:0? -sn -dn ^
  -vf "scale=%W%:%H%:force_original_aspect_ratio=decrease,pad=%W%:%H%:(ow-iw)/2:(oh-ih)/2,fps=%FPS%" ^
  -c:v libx264 -profile:v baseline -level %LVL% -pix_fmt yuv420p ^
  -preset medium -b:v %BV% -maxrate %MAXR% -bufsize %BUF% -g 40 ^
  -c:a aac -b:a 64k -ar 44100 -ac 2 ^
  -movflags +faststart ^
  %OUT%

echo OK: %OUT%
echo Skopiruyte na SD (FAT32). Dlya plavnosti — profil smooth.
