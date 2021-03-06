@REM Copyright 2019 Google LLC
@REM
@REM Licensed under the Apache License, Version 2.0 (the "License");
@REM you may not use this file except in compliance with the License.
@REM You may obtain a copy of the License at
@REM
@REM     http://www.apache.org/licenses/LICENSE-2.0
@REM
@REM Unless required by applicable law or agreed to in writing, software
@REM distributed under the License is distributed on an "AS IS" BASIS,
@REM WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
@REM See the License for the specific language governing permissions and
@REM limitations under the License.

@echo %date% %time%
@echo "Install Bazel using Chocolatey"
choco install --no-progress -y bazel --version 2.0.0
call refreshenv.cmd

@echo %date% %time%
@cd github\google-cloud-cpp

@echo "Create the bazel output directory."
@echo %date% %time%
@if not exist "C:\b\" mkdir C:\b

call "c:\Program Files (x86)\Microsoft Visual Studio\2017\Community\VC\Auxiliary\Build\vcvars64.bat"

@echo %date% %time%
C:\ProgramData\chocolatey\bin\bazel version

@echo "Compiling and running unit tests."
@echo %date% %time%
C:\ProgramData\chocolatey\bin\bazel --output_user_root=C:\b test ^
    --test_output=errors ^
    --verbose_failures=true ^
    --keep_going ^
    -- //google/cloud/...:all

@rem Preserve the exit code of the test for later use because we want to
@rem delete the files in the %KOKORO_ARTIFACTS_DIR% on test failure too.
set test_errorlevel=%errorlevel%

@rem Kokoro rsyncs all the files in the %KOKORO_ARTIFACTS_DIR%, which takes a
@rem long time. The recommended workaround is to remove all the files that are
@rem not interesting artifacts.
echo %date% %time%
cd "%KOKORO_ARTIFACTS_DIR%"
powershell -Command "& {Get-ChildItem -Recurse -File -Exclude test.xml,sponge_log.xml,build.bat | Remove-Item -Recurse -Force}"
if %errorlevel% neq 0 exit /b %errorlevel%

if %test_errorlevel% neq 0 exit /b %test_errorlevel%

@echo %date% %time%
@echo DONE DONE DONE "============================================="
@echo DONE DONE DONE "============================================="
@echo %date% %time%
