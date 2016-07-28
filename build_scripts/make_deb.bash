fpm -n tachyon -v 1.2.7 --prefix /opt/tachyon -C /opt/tachyon -s dir -t deb -p ../../NAME_VERSION_ARCH.deb \
	--after-install deb_post_install \
	--after-remove deb_post_remove
