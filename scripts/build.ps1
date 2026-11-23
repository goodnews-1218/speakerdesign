param([switch]$TeacherAnswer)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$name = if ($TeacherAnswer) { 'build-teacher' } else { 'build-student' }
$answer = if ($TeacherAnswer) { 'ON' } else { 'OFF' }
$build = Join-Path $root $name
& cmake -S $root -B $build -G 'Visual Studio 17 2022' -A x64 "-DCONNECT_LAB_TEACHER_ANSWER=$answer"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& cmake --build $build --config Release
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& ctest --test-dir $build -C Release -R core_wav --output-on-failure
exit $LASTEXITCODE
