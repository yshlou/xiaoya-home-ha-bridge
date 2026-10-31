@echo off
REM ============================================================
REM xbridge cross-compile script (Windows)
REM Prereq: zig on PATH (tested with zig 0.17.0, 0.13+ should work)
REM         https://ziglang.org/download/
REM NOTE: keep this file ASCII-only. cmd.exe parses batch files in
REM       the ANSI codepage (GBK on Chinese Windows); UTF-8 Chinese
REM       comments corrupt parsing.
REM ============================================================
setlocal
set ZIG=zig

where %ZIG% >nul 2>nul
if errorlevel 1 (
    echo [ERROR] zig not found in PATH.
    echo         Install from https://ziglang.org/download/ and add zig.exe folder to PATH,
    echo         or edit the ZIG variable above to the full path of zig.exe.
    exit /b 1
)

echo [1/2] Building aarch64 (for the speaker)...
%ZIG% cc -target aarch64-linux-musl -O2 -DHAVE_MINIMP3 -static -s -o xbridge xbridge.c
if errorlevel 1 ( echo [FAILED] & exit /b 1 )

echo [2/2] Self-check: x86_64 build to validate syntax...
%ZIG% cc -target x86_64-linux-musl -O2 -DHAVE_MINIMP3 -static -s -o xbridge_test xbridge.c
if errorlevel 1 ( echo [FAILED] & exit /b 1 )
del xbridge_test

echo.
echo Done! Output: xbridge (aarch64 static, adb push to speaker)
dir xbridge
