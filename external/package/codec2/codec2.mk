################################################################################
#
# codec2
#
################################################################################

CODEC2_VERSION = 1.2.0
CODEC2_SITE = $(call github,drowe67,codec2,$(CODEC2_VERSION))
CODEC2_LICENSE = LGPL-2.1
CODEC2_LICENSE_FILES = COPYING
CODEC2_INSTALL_STAGING = YES
# Its CMakeLists refuses in-source builds.
CODEC2_SUPPORTS_IN_SOURCE_BUILD = NO
# Cross builds compile the codebook generator for the build machine themselves.
CODEC2_CONF_OPTS = -DUNITTEST=OFF

$(eval $(cmake-package))
