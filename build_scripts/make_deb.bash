fpm -n tachyon -v 1.0 -C /tmp/tachyon -s dir -t deb -p ../NAME_VERSION_ARCH.deb \
	-d "qt5-qtx11extras >= 5.6.0" \
	-d "SDL >= 1.2.15" \
	-d "jack-audio-connection-kit >= 1.9.9" \
	--after-install deb_post_install \
	--after-remove deb_post_remove