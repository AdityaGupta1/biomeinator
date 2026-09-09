#!/usr/bin/env pwsh

# Copyright (c) 2023 NVIDIA CORPORATION. All rights reserved
#
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in all
# copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
# SOFTWARE.

<#
.SYNOPSIS
 Script to take in a file, and convert it to a C header char-array
 representation of its binary contents. Output goes to -o file if provided,
 otherwise to stdout.
.EXAMPLE
 bin2cheader.ps1 -i myfile.spv -o myfile_spv.h
#>

param(
    [Alias("i")]
    [Parameter(Mandatory=$true)]
    [string]
    $inputFilename,

    [Alias("o")]
    [string]
    $outputFilename
)

# Wine cmd doesn't strip outer double quotes from argv when invoking PE
# children, so '"foo.spv"' arrives here verbatim. Trim defensively.
$inputFilename = $inputFilename.Trim('"')
if ($outputFilename) { $outputFilename = $outputFilename.Trim('"') }

$variable_name = $inputFilename.replace(".", "_")

# WAR for `-Encoding Byte` removed in pwsh6 (https://github.com/PowerShell/PowerShell/issues/7986)
if ($PSVersionTable.PSVersion.Major -ge 6) {
    $filevar = Get-Content $inputFilename -AsByteStream
} else {
    $filevar = Get-Content $inputFilename -Encoding Byte
}

$sb = [System.Text.StringBuilder]::new()
[void]$sb.AppendLine("unsigned char $variable_name[] = {")
foreach ($b in $filevar) { [void]$sb.Append("$b, ") }
[void]$sb.AppendLine()
[void]$sb.AppendLine("};")
[void]$sb.AppendLine("unsigned int ${variable_name}_len = $($filevar.Length);")
$output = $sb.ToString()

if ($outputFilename) {
    Set-Content -Path $outputFilename -Value $output -NoNewline
} else {
    Write-Output $output
}
