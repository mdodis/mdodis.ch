@echo off
rem Builds docubuild2 and generates the blog from posts\*.docuspec into ..\blog\
setlocal
pushd %~dp0

if not exist build mkdir build
if not exist ..\blog mkdir ..\blog
cl /nologo /Od /Zi /I . docubuild2.c /Fo:build\ /Fd:build\ /Fe:build\docubuild2.exe || goto :fail
copy /Y static\* ..\blog\ >nul || goto :fail

pushd posts
..\build\docubuild2.exe . -o ..\..\blog -t ..\templates || (popd & goto :fail)
popd

popd
exit /b 0

:fail
popd
exit /b 1
