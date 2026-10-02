@echo off
rem Convert a movie for CamBrowser (ESP32-P4, software H.264 decoder).
rem
rem The SW decoder plays only H.264 Constrained Baseline; 320x180@20 is the
rem realtime ceiling on this board. Output: 320x180, 20 fps, Baseline L2.1,
rem AAC-LC 64k, moov at the front (+faststart).
rem
rem Usage:  convert_movie.bat input.mkv [output.mp4]
rem Requires ffmpeg in PATH (https://www.gyan.dev/ffmpeg/builds/).

if "%~1"=="" (
    echo usage: %~nx0 input.mkv [output.mp4]
    exit /b 1
)
if "%~2"=="" (set OUT=%~dpn1_cam.mp4) else (set OUT=%~2)

ffmpeg -y -i %1 -map 0:v:0 -map 0:a:0? -sn -dn ^
  -vf "scale=320:180:force_original_aspect_ratio=decrease,pad=320:180:(ow-iw)/2:(oh-ih)/2,fps=20" ^
  -c:v libx264 -profile:v baseline -level 2.1 -pix_fmt yuv420p ^
  -preset medium -b:v 250k -maxrate 400k -bufsize 600k -g 40 ^
  -c:a aac -b:a 64k -ar 44100 -ac 2 ^
  -movflags +faststart ^
  %OUT%

echo OK: %OUT%
echo Copy it to the root of the SD card (FAT32) or serve it over HTTP and
echo open it in CamBrowser: VIDEO -^> SVOY URL / SD-KARTA.
