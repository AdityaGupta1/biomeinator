@echo off
set arch_arg=-amd64

:loop
IF NOT "%1"=="" (
	IF "%1"=="-amd64" (
		SET arch_arg=-amd64
		SHIFT
		GOTO :loop
	)
	IF "%1"=="-arm64" (
		SET arch_arg=-arm64
		SHIFT
		GOTO :loop
	)
	IF "%1"=="-aarch64" (
		SET arch_arg=-arm64
		SHIFT
		GOTO :loop
	)
	SHIFT
	GOTO :loop
)

call build.bat %arch_arg% -debug
call build.bat %arch_arg% -develop
call build.bat %arch_arg% -production
