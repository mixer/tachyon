REM check for cef binary
REM check for dependencies (ffmpeg, etc)
SET build_config=Release
SET obs_version=1.2.14
SET cef_binary_dir=C:\beam\cef_binary
SET cef_release_dir=C:\beam\obs-browser-1.22
SET coredeps=C:\beam\tachyon_deps
SET QTDIR64=C:\Qt\5.6\msvc2015_64
SET QTDIR32=C:\Qt\5.6\msvc2015
SET PATH=%PATH%;C:\Program Files (x86)\MSBuild\14.0\Bin;C:\Program Files (x86)\CMake\bin
SET DepsPath32=%coredeps%\win32
SET DepsPath64=%coredeps%\win64
SET build32=
SET build64=
SET package=
if "%1" == "all" (
SET build32=true
SET build64=true
SET package=true
)
if "%1" == "win64" (
SET build64=true
)
if "%1" == "win32" (
SET build32=true
)
echo "building CEF browser plugin"
pushd .
cd ..
call git submodule update --init
cd ..
echo "building libftl"
call git clone https://github.com/WatchBeam/ftl-sdk.git 
cd ftl-sdk
mkdir build
cd build
cmake -G "Visual Studio 14 2015 Win64" ..
call msbuild /t:Rebuild /p:Configuration=%build_config%,Platform=x64 ALL_BUILD.vcxproj || exit /b
SET ftl_lib_dir=%cd%\%build_config%\ftl.lib
SET ftl_inc_dir=%cd%\..\libftl
popd
if defined build64 (
	rmdir CMakeFiles /s /q
	del CMakeCache.txt
	cmake -G "Visual Studio 14 2015 Win64" -DOBS_VERSION_OVERRIDE=%obs_version% -DFTLSDK_LIB=%ftl_lib_dir% -DFTLSDK_INCLUDE_DIR=%ftl_inc_dir% -DCOPIED_DEPENDENCIES=false -DCOPY_DEPENDENCIES=true ..
	call msbuild /p:Configuration=%build_config%,Platform=x64 ALL_BUILD.vcxproj || exit /b
)
if defined build32 (
	rmdir CMakeFiles /s /q
	del CMakeCache.txt
	cmake -G "Visual Studio 14 2015" -DOBS_VERSION_OVERRIDE=%obs_version% -DFTLSDK_LIB=%ftl_lib_dir% -DFTLSDK_INCLUDE_DIR=%ftl_inc_dir% -DCOPIED_DEPENDENCIES=false -DCOPY_DEPENDENCIES=true ..
	call msbuild /p:Configuration=%build_config% ALL_BUILD.vcxproj
	REM Note there will be some errors compiling this as ftl.sdk x86 isnt build but we only need a few binaries from the 32bit version so those errors can be ignored
)
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
