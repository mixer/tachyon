echo @off
SET DepsPath64=C:\beam\dependencies2015\win64
SET FFmpegPath64=C:\beam\dependencies2015\win64
SET x264Path64=C:\beam\dependencies2015\win64
SET curlPath64=C:\beam\dependencies2015\win64
SET QTDIR64=C:\Qt\5.6\msvc2015_64
SET PATH=%PATH%;C:\Program Files (x86)\MSBuild\14.0\Bin
REM Chromium Embedded Framework https://bitbucket.org/chromiumembedded/cef
echo "building CEF browser plugin"
REM call python automate-git.py --download-dir=../.. --branch=2623 --x64-build
pushd .
cd ..\..\
REM windows 64 bit binary: https://cefbuilds.com/#branch_2623
REM SET cef_root_dir=%cd%\cef
SET cef_root_dir=%cd%\cef_binary
REM cd cef_binary
REM mkdir build
REM cd build
REM cmake -G "Visual Studio 14 2015 Win64" ..
REM msbuild /p:Configuration=Release,Platform=x64 ALL_BUILD.vcxproj
REM /machine is set incorrectly should be x64
REM cd..\..
echo "building libftl"
call git clone https://github.com/WatchBeam/ftl-sdk.git
cd ftl-sdk
mkdir build
cd build
cmake -G "Visual Studio 14 2015 Win64" ..
REM Microsoft Build Tools 2015 https://www.microsoft.com/en-us/download/details.aspx?id=4815
call msbuild /t:Rebuild /p:Configuration=Release,Platform=x64 ftl.vcxproj
SET ftl_lib_dir=%cd%\Release\ftl.lib
SET ftl_inc_dir=%cd%\..\libftl
popd
cmake -G "Visual Studio 14 2015 Win64" -DFTLSDK_LIB=%ftl_lib_dir% -DFTLSDK_INCLUDE_DIR=%ftl_inc_dir% -DCEF_ROOT_DIR=%cef_root_dir% -DCOPY_DEPENDENCIES=true ..
