REM Set launch params below, or in steam
SET params=-w 1280 -h 720 -novid -condebug -windowed +developer 1 -high -refresh 0 -noasserts -hushasserts -steam -game tfvr

SET DXVK_STATE_CACHE=1
SET TFVR_STATE_CACHE_PATH=%~dp0%dxvk-cache
SET DXVK_ASYNC=1
SET DXVK_LOG_LEVEL=warn
REM start the game
START tfvr_win64.exe %* %params%