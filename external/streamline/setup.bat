@echo off
set cfg=
set arch=amd64
set packman_platform=windows-x86_64
set premake_args=
:loop
IF NOT "%1"=="" (
    IF "%1"=="vs2019" (
        SET cfg=vs2019
        SHIFT
        GOTO :loop
    )
    IF "%1"=="vs2022" (
        SET cfg=vs2022
        SHIFT
        GOTO :loop
    )
    IF "%1"=="amd64" (
        SET arch=amd64
        SET packman_platform=windows-x86_64
        SHIFT
        GOTO :loop
    )
    IF "%1"=="arm64" (
        SET arch=arm64
        SET packman_platform=windows-aarch64
        SHIFT
        GOTO :loop
    )
    IF "%1"=="aarch64" (
        SET arch=arm64
        SET packman_platform=windows-aarch64
        SHIFT
        GOTO :loop
    )
    IF "%1"=="arm64ec" (
        SET arch=arm64ec
        SET packman_platform=windows-x86_64
        SHIFT
        GOTO :loop
    )
    SET premake_args=%premake_args% %1
    SHIFT
    GOTO :loop
)

IF "%cfg%"=="" (
    IF exist .\_project\vs2022\streamline.sln (
        SET cfg=vs2022
    ) ELSE IF exist .\_project\vs2019\streamline.sln (
        SET cfg=vs2019
    ) ELSE (
        exit /b 1
    )
)

echo Creating project files for %cfg%
call .\tools\packman\packman.cmd pull -p %packman_platform% project.xml
call .\tools\premake5\premake5.exe %cfg% --file=.\premake.lua %premake_args%
