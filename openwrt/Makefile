include $(TOPDIR)/rules.mk

PKG_NAME:=tcpredir
PKG_VERSION:=1.2.0
PKG_RELEASE:=1

# The release tarball is built recursively (submodules included) by the repo's
# release workflow, so it carries the vendored C++ libraries.
PKG_SOURCE:=$(PKG_NAME)-$(PKG_VERSION).tar.gz
PKG_SOURCE_URL:=https://github.com/oskarirauta/$(PKG_NAME)/releases/download/$(PKG_VERSION)/
PKG_HASH:=skip

PKG_LICENSE:=MIT
PKG_LICENSE_FILES:=LICENSE
PKG_MAINTAINER:=Oskari Rauta <oskari.rauta@gmail.com>

include $(INCLUDE_DIR)/package.mk

define Package/tcpredir
  SECTION:=net
  CATEGORY:=Network
  TITLE:=Userspace TCP/UDP port redirector
  URL:=https://github.com/oskarirauta/tcpredir
  # libstdcpp: this is C++ (g++), so it links libstdc++.so. UCI is read by a
  # vendored C++ parser rather than libuci, so libuci is deliberately absent.
  DEPENDS:=+libubox +libubus +libblobmsg-json +libstdcpp
endef

define Package/tcpredir/description
  tcpredir forwards a port on this router to an address and port elsewhere -
  a container's private netns address, a host on another subnet, a machine
  reachable only from the router. It is a userspace proxy, not a firewall
  rule: the target sees the router as the client, which is the price of
  reaching a place fw4 cannot redirect to.

  Redirects come from /etc/config/tcpredir, or as command-line arguments from
  whoever starts the process. In config mode it serves a ubus object
  ('tcpredir') for listing redirects and adding, removing or reloading them at
  runtime. SIGHUP (or `/etc/init.d/tcpredir reload`) re-reads the file and
  restarts only the redirects that came from it, so established connections
  through the others survive an edit.
endef

# /etc/config/tcpredir is preserved across sysupgrade.
define Package/tcpredir/conffiles
/etc/config/tcpredir
endef

define Build/Configure
endef

define Build/Compile
	$(MAKE) -C $(PKG_BUILD_DIR) \
		CXX="$(TARGET_CXX)" \
		CXXFLAGS="$(TARGET_CXXFLAGS) $(TARGET_CPPFLAGS) --std=c++20 -fPIC" \
		LDFLAGS="$(TARGET_LDFLAGS)" \
		tcpredir
endef

define Package/tcpredir/install
	$(INSTALL_DIR) $(1)/usr/sbin
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/tcpredir $(1)/usr/sbin/tcpredir

	$(INSTALL_DIR) $(1)/etc/init.d
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/openwrt.init $(1)/etc/init.d/tcpredir

	$(INSTALL_DIR) $(1)/etc/config
	$(INSTALL_CONF) $(PKG_BUILD_DIR)/openwrt.config $(1)/etc/config/tcpredir
endef

define Package/luci-app-tcpredir
  SECTION:=luci
  CATEGORY:=LuCI
  SUBMENU:=3. Applications
  TITLE:=Port redirect UI for tcpredir
  DEPENDS:=+luci-base +tcpredir
  PKGARCH:=all
endef

define Package/luci-app-tcpredir/description
  LuCI web interface for tcpredir: a "Port redirects" page editing
  /etc/config/tcpredir, and a Status page showing every redirect the daemon is
  actually serving, each labelled with where it came from - the configuration
  file, or added at runtime over ubus. Saving applies with a reload rather than
  a restart, so runtime redirects and established connections survive an edit.

  If uxcd is installed, the Status page also shows the host ports its containers
  publish, read-only and in their own section: those are decided by the
  container's configuration, not here.
endef

define Package/luci-app-tcpredir/install
	$(INSTALL_DIR) $(1)/www/luci-static/resources/view/tcpredir
	$(INSTALL_DATA) $(PKG_BUILD_DIR)/luci/app-tcpredir/htdocs/luci-static/resources/view/tcpredir/redirects.js \
		$(1)/www/luci-static/resources/view/tcpredir/redirects.js
	$(INSTALL_DATA) $(PKG_BUILD_DIR)/luci/app-tcpredir/htdocs/luci-static/resources/view/tcpredir/status.js \
		$(1)/www/luci-static/resources/view/tcpredir/status.js

	$(INSTALL_DIR) $(1)/usr/share/luci/menu.d
	$(INSTALL_DATA) $(PKG_BUILD_DIR)/luci/app-tcpredir/root/usr/share/luci/menu.d/luci-app-tcpredir.json \
		$(1)/usr/share/luci/menu.d/luci-app-tcpredir.json

	$(INSTALL_DIR) $(1)/usr/share/rpcd/acl.d
	$(INSTALL_DATA) $(PKG_BUILD_DIR)/luci/app-tcpredir/root/usr/share/rpcd/acl.d/luci-app-tcpredir.json \
		$(1)/usr/share/rpcd/acl.d/luci-app-tcpredir.json
endef

$(eval $(call BuildPackage,tcpredir))
$(eval $(call BuildPackage,luci-app-tcpredir))
