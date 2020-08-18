mark_as_advanced( FORCE APPLICATION_NAME APPLICATION_SHORTNAME APPLICATION_EXECUTABLE APPLICATION_DOMAIN APPLICATION_VENDOR APPLICATION_ICON_NAME APPLICATION_REV_DOMAIN LINUX_PACKAGE_SHORTNAME THEME_CLASS )

set( APPLICATION_NAME       "Nextcloud" CACHE STRING "Name of the application, visible in title bar" )
set( APPLICATION_SHORTNAME  "Nextcloud" CACHE STRING "Short name of the application" )
set( APPLICATION_EXECUTABLE "nextcloud" CACHE STRING "Name of the application executable" )
set( APPLICATION_DOMAIN     "nextcloud.com" CACHE STRING "Domain of the application provider" )
set( APPLICATION_VENDOR     "Nextcloud GmbH" CACHE STRING "Name of the application vendor" )
set( APPLICATION_UPDATE_URL "https://updates.nextcloud.org/client/" CACHE STRING "URL for updater" )
set( APPLICATION_HELP_URL   "" CACHE STRING "URL for the help menu" )
set( APPLICATION_ICON_NAME  "Nextcloud" CACHE STRING "Name of the application icon" )
set( APPLICATION_SERVER_URL "" CACHE STRING "URL for the server to use. If entered the server can only connect to this instance" )
set( APPLICATION_REV_DOMAIN "com.nextcloud.desktopclient" CACHE STRING "Name of the application rev-domain" )


# linux specific
set( LINUX_PACKAGE_SHORTNAME "nextcloud" CACHE STRING "Short name of the linux package" )
set( LINUX_APPLICATION_ID "${APPLICATION_REV_DOMAIN}.${LINUX_PACKAGE_SHORTNAME}")


# windows specific
set( WIN_SETUP_BITMAP_PATH  "${CMAKE_SOURCE_DIR}/admin/win/nsi" )


# mac specific
set( MAC_INSTALLER_BACKGROUND_FILE "${CMAKE_SOURCE_DIR}/admin/osx/installer-background.png" CACHE STRING "The MacOSX installer background image")


# crash reporter options
option( WITH_CRASHREPORTER      "Build crashreporter" OFF)
#set( CRASHREPORTER_SUBMIT_URL  "https://crash-reports.owncloud.com/submit" CACHE STRING "URL for crash reporter" )
#set( CRASHREPORTER_ICON        ":/owncloud-icon.png" )


## updater options
option( BUILD_UPDATER  "Build updater" OFF )
option( WITH_PROVIDERS "Build with providers list" ON )


## theming options
set( THEME_CLASS                                "NextcloudTheme" CACHE STRING "Name of the theme class" )
#set( THEME_INCLUDE                             "${OEM_THEME_DIR}/mytheme.h" )
set( APPLICATION_WIZARD_HEADER_BACKGROUND_COLOR "#0082c9" CACHE STRING "Hex color of the wizard header background")
set( APPLICATION_WIZARD_HEADER_TITLE_COLOR      "#ffffff" CACHE STRING "Hex color of the text in the wizard header")
option( APPLICATION_WIZARD_USE_CUSTOM_LOGO       "Use the logo from ':/client/theme/colored/wizard_logo.png' else the default application icon is used" ON )
