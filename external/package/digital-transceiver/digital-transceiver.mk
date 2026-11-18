################################################################################
#
# digital-transceiver
#
################################################################################

# The app's source is in this project (app/), so every build uses the current tree.
DIGITAL_TRANSCEIVER_VERSION = local
DIGITAL_TRANSCEIVER_SITE = $(BR2_EXTERNAL_DTRX_PATH)/../app
DIGITAL_TRANSCEIVER_SITE_METHOD = local
DIGITAL_TRANSCEIVER_LICENSE = GPL-3.0+
DIGITAL_TRANSCEIVER_LICENSE_FILES = LICENSE
DIGITAL_TRANSCEIVER_DEPENDENCIES = qt6base codec2
DIGITAL_TRANSCEIVER_SUPPORTS_IN_SOURCE_BUILD = NO
# A desktop test build in app/build is not part of the source.
DIGITAL_TRANSCEIVER_OVERRIDE_SRCDIR_RSYNC_EXCLUSIONS = --exclude /build

define DIGITAL_TRANSCEIVER_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(DIGITAL_TRANSCEIVER_BUILDDIR)/digital-transceiver \
		$(TARGET_DIR)/usr/bin/digital-transceiver
endef

$(eval $(cmake-package))
