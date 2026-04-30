@echo off
setlocal

rem Build custom shaders for TF2VR (includes vgui_rounded and spritecard)

set BUILD_SHADER=call buildshaders.bat
set GAME_DIR="..\..\..\game\tfvr"
set SRC_DIR="..\..\"

echo Building custom shaders for TF2VR...
echo This will compile: vgui_rounded, spritecard
echo.

rem Build SDK shaders which includes our custom shaders
%BUILD_SHADER% sdkshaders_dx9_20b -game %GAME_DIR% -source %SRC_DIR%

echo.
echo Done! The compiled .vcs files should be in game\tfvr\shaders\fxc\
pause
