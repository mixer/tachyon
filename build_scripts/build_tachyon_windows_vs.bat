REM check for cef binary
REM check for dependencies (ffmpeg, etc)
SET build_config=Release
SET obs_version=1.2.8
SET cef_binary_dir=C:\beam\cef_binary
SET cef_release_dir=C:\beam\obs-browser-1.22
SET coredeps=C:\beam\tachyon_deps
SET QTDIR64=C:\Qt\5.6\msvc2015_64
SET PATH=%PATH%;C:\Program Files (x86)\MSBuild\14.0\Bin;C:\Program Files (x86)\CMake\bin
SET DepsPath64=%coredeps%\win64
SET FFmpegPath64=%coredeps%\win64
SET x264Path64=%coredeps%\win64
SET curlPath64=%coredeps%\win64
echo "building CEF browser plugin"
REM call python automate-git.py --download-dir=../.. --branch=2623 --x64-build
REM SET cef_root_dir=%cd%\cef
pushd .
cd ..
call git submodule update --init
cd ..
REM windows 64 bit binary: https://cefbuilds.com/#branch_2623
REM pushd .
REM cd %cef_binary_dir%
REM mkdir build
REM cd build
REM cmake -G "Visual Studio 14 2015 Win64" ..
REM msbuild /p:Configuration=%build_config%,Platform=x64 ALL_BUILD.vcxproj || exit /b
REM current CEF plugin expects this one folder down
REM copy libcef_dll\%build_config%\libcef_dll_wrapper.lib libcef_dll\
REM popd
echo "building libftl"
call git clone https://github.com/WatchBeam/ftl-sdk.git
cd ftl-sdk
mkdir build
cd build
cmake -G "Visual Studio 14 2015 Win64" ..
call msbuild /t:Rebuild /p:Configuration=%build_config%,Platform=x64 ftl.vcxproj || exit /b
SET ftl_lib_dir=%cd%\%build_config%\ftl.lib
SET ftl_inc_dir=%cd%\..\libftl
popd
REM cmake -G "Visual Studio 14 2015 Win64" -DOBS_VERSION_OVERRIDE=%obs_version% -DFTLSDK_LIB=%ftl_lib_dir% -DFTLSDK_INCLUDE_DIR=%ftl_inc_dir% -DCEF_ROOT_DIR=%cef_binary_dir% -DCOPY_DEPENDENCIES=true ..
cmake -G "Visual Studio 14 2015 Win64" -DOBS_VERSION_OVERRIDE=%obs_version% -DFTLSDK_LIB=%ftl_lib_dir% -DFTLSDK_INCLUDE_DIR=%ftl_inc_dir% -DCOPY_DEPENDENCIES=true ..
call msbuild /p:Configuration=%build_config%,Platform=x64 ALL_BUILD.vcxproj || exit /b
echo "Building FTL-Express"
pushd .
cd ..\..
call git clone https://github.com/WatchBeam/ftl-express.git
cd ftl-express
SET GOOS=windows
go get
go build
copy ftl-express.exe %coredeps%\win64\bin\  || exit /b
popd
echo "Copying Browser plugin"
REM xcopy %cef_binary_dir%\Resources\* rundir\%build_config%\obs-plugins\64bit\ /s /e /y
xcopy %cef_release_dir%\obs-plugins\64bit\* rundir\%build_config%\obs-plugins\64bit\ /s /e /y
copy %cef_binary_dir%\%build_config%\d3dcompiler_43.dll rundir\%build_config%\obs-plugins\64bit\
copy %cef_binary_dir%\%build_config%\d3dcompiler_47.dll rundir\%build_config%\obs-plugins\64bit\
REM copy %cef_binary_dir%\%build_config%\libcef.dll rundir\%build_config%\obs-plugins\64bit\
REM copy %cef_binary_dir%\%build_config%\natives_blob.bin rundir\%build_config%\obs-plugins\64bit\
REM copy %cef_binary_dir%\%build_config%\snapshot_blob.bin rundir\%build_config%\obs-plugins\64bit\
copy %coredeps%\win64\bin\postproc-54.dll rundir\%build_config%\bin\64bit
copy %coredeps%\win64\bin\ftl-express.exe rundir\%build_config%\bin\64bit
