#!/bin/bash

. scripts/lib.sh

curl -fL "https://libsdl.org/release/SDL2-devel-$SDL_VERSION-VC.zip" -o SDL2.zip || die
unzip -q SDL2.zip || die
mv "SDL2-$SDL_VERSION" SDL2_VC || die
rm -f SDL2.zip

if [ "$GH_CPU_ARCH" = "i386" ]; then
	rustup target add i686-pc-windows-msvc

	# YY-Thunks obj for Windows XP support
	curl -fL "https://github.com/Chuyu-Team/YY-Thunks/releases/download/$YY_THUNKS_VERSION/YY-Thunks-Objs.zip" -o yy-thunks.zip || die
	unzip -q -j -o yy-thunks.zip 'objs/x86/YY_Thunks_for_WinXP.obj' -d 3rdparty/yy-thunks || die
	rm yy-thunks.zip
fi

curl -fL https://github.com/FWGS/potential-meme/releases/download/prebuilts/mingw-w64-x86_64-pkgconf-1.2.3.0-1-any.pkg.tar.zst -o pkgconf.tar.zst || die
7z x pkgconf.tar.zst || die
7z x pkgconf.tar || die
rm pkgconf.tar*
mv mingw64 pkgconf || die

FFMPEG_ARCHIVE=$(get_ffmpeg_archive)
curl -fL "https://github.com/FWGS/FFmpeg-Builds/releases/download/latest/$FFMPEG_ARCHIVE.zip" -o ffmpeg.zip
if [ -f ffmpeg.zip ]; then
	unzip -x ffmpeg.zip
	mv "$FFMPEG_ARCHIVE" ffmpeg
fi
