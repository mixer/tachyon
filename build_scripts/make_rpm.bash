fpm --rpm-summary "Tachyon OBS-Studio Fork with FTL (faster than light)" \
	-n tachyon -v 1.1 --prefix /opt/tachyon -C /opt/tachyon -s dir -t rpm -p ../../NAME_VERSION_ARCH.rpm \
	--after-install rpm_post_install \
	--after-remove rpm_post_remove
	
