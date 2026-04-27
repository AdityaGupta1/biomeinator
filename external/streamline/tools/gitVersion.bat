@echo off
setlocal enabledelayedexpansion

git log -1 --pretty=format:"SHA:%%x20%%h" > tmpFile 
set /p cl=<tmpFile
del tmpFile
git log -1 --pretty=format:"%%h" > tmpFile 
set /p cls=<tmpFile
del tmpFile
set "cltag="
git describe --tags --match "v*" --exact-match > tmpFile 2>nul
if not errorlevel 1 (
    set /p cltag=<tmpFile
)
del tmpFile
del gitVersion.h /f
echo #ifndef _GIT_VERSION_HEADER >> gitVersion.h
echo #define _GIT_VERSION_HEADER >> gitVersion.h
echo //! Auto generated - start >> gitVersion.h
echo #define GIT_LAST_COMMIT "%cl%" >> gitVersion.h
echo #define GIT_LAST_COMMIT_TAG "%cltag%" >> gitVersion.h
echo #define GIT_LAST_COMMIT_SHORT "%cls%" >> gitVersion.h
echo //! Auto generated - end >> gitVersion.h
echo #endif >> gitVersion.h
