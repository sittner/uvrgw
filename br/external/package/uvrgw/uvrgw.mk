################################################################################
#
# uvrgw
#
################################################################################

# built from the working tree this external tree belongs to
UVRGW_VERSION = local
UVRGW_SITE = $(BR2_EXTERNAL_UVRGW_PATH)/../..
UVRGW_SITE_METHOD = local
UVRGW_DEPENDENCIES = libconfuse libmodbus mosquitto libcurl json-c
UVRGW_LICENSE = GPL-3.0+, Zlib (tinyexpr)
UVRGW_LICENSE_FILES = LICENSE tinyexpr/LICENSE

# no build results of the host, no production config, no image build tree
UVRGW_OVERRIDE_SRCDIR_RSYNC_EXCLUSIONS = \
	--exclude='*.o' --exclude=/uvrgw --exclude=/uvrgw.conf \
	--exclude=/br --exclude=/test

define UVRGW_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(MAKE) -C $(@D) CC="$(TARGET_CC)" \
		CFLAGS="$(TARGET_CFLAGS) -I. -Wall" LDFLAGS="$(TARGET_LDFLAGS)"
endef

# installs the binary and uvrgw.service
define UVRGW_INSTALL_TARGET_CMDS
	$(TARGET_MAKE_ENV) $(MAKE) -C $(@D) DESTDIR=$(TARGET_DIR) install
endef

define UVRGW_USERS
	uvrgw -1 uvrgw -1 * - - - uvrgw gateway
endef

$(eval $(generic-package))
