NOTE:  If you run into problems don't get frustrated, reach out to us at beam.pro/contact

#######################
#     WINDOWS 64      #
#######################
Note:  These steps are to build the 64bit version of Tachyon.  If you need 32 bit you're on your own for the forseeable future

dependancies
############

The build script expects a few things:
*Visual Studio 2015 (Community is perfectly fine): https://www.visualstudio.com/en-us/downloads/download-visual-studio-vs.aspx
**IMPORTANT: make sure during the install the following is checked: under features->programming languages->visual c++->common tools for visual c++ 2015
**IMPORTANT: if you forgot to do this during the install you can go to add/remove programs and select modify and enable it the same as above
*MSBuild tool: https://www.microsoft.com/en-us/download/details.aspx?id=48159
*CEF Browser plugin - you can change the install location but the script expect it in c:\beam\cef_binary : https://cefbuilds.com/ Branch 2623 Windows 64bit (CEF 3.2623.1401.gb90a3be (123MB)) 
*additional dependancies including vp8, opus, ffmpeg, curl, (and a few others) to be installed C:\beam\tachyon_deps : https://github.com/WatchBeam/tachyon/releases/download/v1.1.0/tachyon_deps.zip
*cmake https://cmake.org/
*QT 5.6 https://www.qt.io/download/ (select the open source version)
*NSIS (only needed to build the installer package) http://nsis.sourceforge.net/

building:
#########
git clone https://github.com/WatchBeam/tachyon.git
cd tachyon
git checkout ftl-ffmpeg
cd build_scripts
build_tachyon_windows_vs.bat
*once complete everything should be located in tachyon\build_scripts\rundir\Release

Installer
#########
*after compleleting the above step run:
create_tachyon_installer.bat

Tachyon_Installer_1.1.0.exe will be placed in the same folder


#######################
#     Linux   64      #
#######################
Note:  These steps are to build the 64bit version of Tachyon.  If you need 32 bit you're on your own for the forseeable future

Note 2: In general the linux dependancies are identical to those of OBS Studio: https://github.com/jp9000/obs-studio/wiki/Install-Instructions

building
#########
git clone https://github.com/WatchBeam/tachyon.git
cd tachyon
git checkout ftl-ffmpeg
cd build_scripts
sudo ./build_tachyon_linux

creating an RPM package
#######################
Note: the rpm script expects tachyon to be installed in /opt/tachyon which is where the build_tachyon_linux script puts it

sudo ./make_rpm.bash

creating an DEB package
#######################
Note: the deb script expects tachyon to be installed in /opt/tachyon which is where the build_tachyon_linux script puts it

sudo ./make_deb.bash




