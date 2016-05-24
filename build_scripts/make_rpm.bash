fpm --rpm-summary "Tachyon OBS-Studio Fork with FTL (faster than light)" \
	-n tachyon -v 1.1 --prefix /opt/tachyon -C /opt/tachyon -s dir -t rpm -p ../../NAME_VERSION_ARCH.rpm \
	-d "qt5-qtx11extras >= 5.6.0" \
	-d "SDL >= 1.2.15" \
	-d "jack-audio-connection-kit >= 1.9.9" \
	--after-install rpm_post_install \
	--after-remove rpm_post_remove
	
