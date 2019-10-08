project(gui)
find_package(Qt5 REQUIRED COMPONENTS Widgets)
set(CMAKE_AUTOMOC TRUE)
set(CMAKE_AUTOUIC TRUE)
set(CMAKE_AUTORCC TRUE)
add_subdirectory(updater)

#TODO Move resources files
set(MIRALL_RC_SRC ../../client.qrc)
if (EXISTS "${OEM_THEME_DIR}/theme.qrc")
    list(APPEND MIRALL_RC_SRC ${OEM_THEME_DIR}/theme.qrc)
    set(theme_dir ${OEM_THEME_DIR}/theme)
else()
    list(APPEND MIRALL_RC_SRC ../../theme.qrc)
    set(theme_dir ${CMAKE_SOURCE_DIR}/theme)
endif()

set(client_UI_SRCS
    accountsettings.ui
    folderwizardsourcepage.ui
    folderwizardtargetpage.ui
    generalsettings.ui
    legalnotice.ui
    ignorelisteditor.ui
    ignorelisttablewidget.ui
	wifilistmanager.ui
    networksettings.ui
    activitywidget.ui
    synclogdialog.ui
    settingsdialog.ui
    sharedialog.ui
    sharelinkwidget.ui
    shareusergroupwidget.ui
    shareuserline.ui
    sslerrordialog.ui
    addcertificatedialog.ui
    proxyauthdialog.ui
    mnemonicdialog.ui
    wizard/owncloudadvancedsetuppage.ui
    wizard/owncloudconnectionmethoddialog.ui
    wizard/owncloudhttpcredspage.ui
    wizard/owncloudoauthcredspage.ui
    wizard/flow2authcredspage.ui
    wizard/flow2authwidget.ui
    wizard/owncloudsetupnocredspage.ui
    wizard/owncloudwizardresultpage.ui
    wizard/webview.ui
)

set(client_SRCS
    accountmanager.cpp
    accountsettings.cpp
    application.cpp
    clientproxy.cpp
    connectionvalidator.cpp
    folder.cpp
    folderman.cpp
    folderstatusmodel.cpp
    folderstatusdelegate.cpp
    folderstatusview.cpp
    folderwatcher.cpp
    folderwizard.cpp
    generalsettings.cpp
    legalnotice.cpp
    ignorelisteditor.cpp
    ignorelisttablewidget.cpp
    lockwatcher.cpp
    logbrowser.cpp
    navigationpanehelper.cpp
	wifilistmanager.cpp
    networksettings.cpp
    ocsnavigationappsjob.cpp
    ocsjob.cpp
    ocssharejob.cpp
    ocsshareejob.cpp
    openfilemanager.cpp
    owncloudgui.cpp
    owncloudsetupwizard.cpp
    activitydata.cpp
    activitylistmodel.cpp
    activitywidget.cpp
    activityitemdelegate.cpp
    selectivesyncdialog.cpp
    settingsdialog.cpp
    sharedialog.cpp
    sharelinkwidget.cpp
    sharemanager.cpp
    shareusergroupwidget.cpp
    sharee.cpp
    socketapi.cpp
    sslbutton.cpp
    sslerrordialog.cpp
    syncrunfilelog.cpp
    systray.cpp
    thumbnailjob.cpp
    quotainfo.cpp
    accountstate.cpp
    addcertificatedialog.cpp
    authenticationdialog.cpp
    proxyauthhandler.cpp
    proxyauthdialog.cpp
    synclogdialog.cpp
    tooltipupdater.cpp
    notificationconfirmjob.cpp
    servernotificationhandler.cpp
    guiutility.cpp
    elidedlabel.cpp
    iconjob.cpp
    creds/credentialsfactory.cpp
    creds/httpcredentialsgui.cpp
    creds/oauth.cpp
    creds/flow2auth.cpp
    creds/webflowcredentials.cpp
    creds/webflowcredentialsdialog.cpp
    wizard/postfixlineedit.cpp
    wizard/abstractcredswizardpage.cpp
    wizard/owncloudadvancedsetuppage.cpp
    wizard/owncloudconnectionmethoddialog.cpp
    wizard/owncloudhttpcredspage.cpp
    wizard/owncloudoauthcredspage.cpp
    wizard/flow2authcredspage.cpp
    wizard/flow2authwidget.cpp
    wizard/owncloudsetuppage.cpp
    wizard/owncloudwizardcommon.cpp
    wizard/owncloudwizard.cpp
    wizard/owncloudwizardresultpage.cpp
    wizard/webviewpage.cpp
    wizard/webview.cpp
    wizard/slideshow.cpp
)

IF(NOT NO_SHIBBOLETH)
    list(APPEND client_SRCS
        creds/shibbolethcredentials.cpp
        creds/shibboleth/shibbolethwebview.cpp
        creds/shibboleth/shibbolethuserjob.cpp
        wizard/owncloudshibbolethcredspage.cpp
    )
endif()

set(updater_SRCS
    updater/ocupdater.cpp
    updater/updateinfo.cpp
    updater/updater.cpp
)

IF( APPLE )
    list(APPEND client_SRCS cocoainitializer_mac.mm)
    list(APPEND client_SRCS settingsdialogmac.cpp)
    list(REMOVE_ITEM client_SRCS settingsdialog.cpp)
    list(APPEND client_SRCS socketapisocket_mac.mm)
    list(APPEND client_SRCS systray.mm)

    if(SPARKLE_FOUND)
       # Define this, we need to check in updater.cpp
       add_definitions( -DHAVE_SPARKLE )
       list(APPEND updater_SRCS updater/sparkleupdater_mac.mm)
   endif()
ENDIF()

IF( NOT WIN32 AND NOT APPLE )
set(client_SRCS ${client_SRCS} folderwatcher_linux.cpp)
ENDIF()
IF( WIN32 )
set(client_SRCS ${client_SRCS} folderwatcher_win.cpp)
ENDIF()
IF( APPLE )
list(APPEND client_SRCS folderwatcher_mac.cpp)
ENDIF()

set(3rdparty_SRC
    ../3rdparty/QProgressIndicator/QProgressIndicator.cpp
    ../3rdparty/qtlockedfile/qtlockedfile.cpp
    ../3rdparty/qtsingleapplication/qtlocalpeer.cpp
    ../3rdparty/qtsingleapplication/qtsingleapplication.cpp
    ../3rdparty/qtsingleapplication/qtsinglecoreapplication.cpp
    ../3rdparty/kmessagewidget/kmessagewidget.cpp
   )

if (APPLE)
    list(APPEND 3rdparty_SRC
        ../3rdparty/qtmacgoodies/src/macpreferenceswindow.mm
        ../3rdparty/qtmacgoodies/src/macstandardicon.mm
        ../3rdparty/qtmacgoodies/src/macwindow.mm
        )
endif()

if(NOT WIN32)
   list(APPEND 3rdparty_SRC ../3rdparty/qtlockedfile/qtlockedfile_unix.cpp)
else()
   list(APPEND 3rdparty_SRC ../3rdparty/qtlockedfile/qtlockedfile_win.cpp )
endif()

IF( NOT WIN32 AND NOT APPLE AND LIBCLOUDPROVIDERS_FOUND)
    message("Building with libcloudproviderssupport")
    add_definitions(-DWITH_LIBCLOUDPROVIDERS)
    set(client_SRCS ${client_SRCS} cloudproviders/cloudprovidermanager.cpp)
    set(client_SRCS ${client_SRCS} cloudproviders/cloudproviderwrapper.cpp)

    include_directories(${GLib2_INCLUDE_DIRS})
    include_directories(${GIO_INCLUDE_DIRS})
    include_directories(${LIBCLOUDPROVIDERS_INCLUDE_DIR})
ENDIF()

find_package(Qt5LinguistTools)
if(Qt5LinguistTools_FOUND)
    qt5_add_translation(client_I18N ${TRANSLATIONS})
endif()

IF( WIN32 )
    configure_file(
      ${CMAKE_CURRENT_SOURCE_DIR}/version.rc.in
      ${CMAKE_CURRENT_BINARY_DIR}/version.rc
      @ONLY)
    set(client_version ${CMAKE_CURRENT_BINARY_DIR}/version.rc)
    IF(NOT MSVC)
        set(client_manifest ${CMAKE_CURRENT_SOURCE_DIR}/manifest-mingw.rc)
    ENDIF()
ENDIF()

set( final_src
    ${MIRALL_RC_SRC}
    ${client_SRCS}
    ${client_UI_SRCS}
    ${client_version}
    ${client_manifest}
    ${guiMoc}
    ${client_I18N}
    ${3rdparty_SRC}
    ${3rdparty_MOC}
)

if(QTKEYCHAIN_FOUND OR QT5KEYCHAIN_FOUND)
    list(APPEND libsync_LINK_TARGETS ${QTKEYCHAIN_LIBRARY})
    include_directories(${QTKEYCHAIN_INCLUDE_DIR})
endif()

# add executable icon on windows and osx

# UPSTREAM our ECMAddAppIcon.cmake then require that version here
# find_package(ECM 1.7.0 REQUIRED NO_MODULE)
# list(APPEND CMAKE_MODULE_PATH ${ECM_MODULE_PATH})
include(ECMAddAppIcon)

# For historical reasons we can not use the application_shortname
# for ownCloud but must rather set it manually.
if (NOT DEFINED APPLICATION_ICON_NAME)
    set(APPLICATION_ICON_NAME ${APPLICATION_SHORTNAME})
endif()

file(GLOB_RECURSE OWNCLOUD_ICONS "${theme_dir}/colored/*-${APPLICATION_ICON_NAME}-icon*")
if(APPLE)
    file(GLOB_RECURSE OWNCLOUD_SIDEBAR_ICONS "${theme_dir}/colored/*-${APPLICATION_ICON_NAME}-sidebar*")
    MESSAGE(STATUS "OWNCLOUD_SIDEBAR_ICONS: ${APPLICATION_ICON_NAME}: ${OWNCLOUD_SIDEBAR_ICONS}")
endif()
ecm_add_app_icon(final_src ICONS "${OWNCLOUD_ICONS}" SIDEBAR_ICONS "${OWNCLOUD_SIDEBAR_ICONS}" OUTFILE_BASENAME "${APPLICATION_ICON_NAME}")

if(UNIX AND NOT APPLE)
  set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fPIE")
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fPIE")
  set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -pie")
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "FreeBSD")
  set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -L/usr/local/lib")
endif()

if(NOT BUILD_OWNCLOUD_OSX_BUNDLE)
    if(NOT WIN32)
        file(GLOB _icons "${theme_dir}/colored/*-${APPLICATION_ICON_NAME}-icon.png")
        foreach(_file ${_icons})
            string(REPLACE "${theme_dir}/colored/" "" _res ${_file})
            string(REPLACE "-${APPLICATION_ICON_NAME}-icon.png" "" _res ${_res})
            install(FILES ${_file} RENAME ${APPLICATION_ICON_NAME}.png DESTINATION ${DATADIR}/icons/hicolor/${_res}x${_res}/apps)
        endforeach(_file)
        install(FILES ${client_I18N} DESTINATION ${SHAREDIR}/${APPLICATION_EXECUTABLE}/i18n)
    else()
        file(GLOB_RECURSE VISUAL_ELEMENTS "${theme_dir}/colored/*-${APPLICATION_ICON_NAME}-w10startmenu*")
        install(FILES ${VISUAL_ELEMENTS} DESTINATION bin/visualelements)
        install(FILES "${theme_dir}/${APPLICATION_EXECUTABLE}.VisualElementsManifest.xml" DESTINATION bin)
        install(FILES ${client_I18N} DESTINATION i18n)
    endif()

    # we may not add MACOSX_BUNDLE here, if not building one

    # add_executable( ${APPLICATION_EXECUTABLE} main.cpp ${final_src})
    add_executable( ${APPLICATION_EXECUTABLE} WIN32 main.cpp ${final_src})
else()
    # set(CMAKE_INSTALL_PREFIX ".") # Examples use /Applications. hurmpf.
    set(MACOSX_BUNDLE_ICON_FILE "${APPLICATION_ICON_NAME}.icns")

    # we must add MACOSX_BUNDLE only if building a bundle
    add_executable( ${APPLICATION_EXECUTABLE} WIN32 MACOSX_BUNDLE main.cpp ${final_src})

    set (QM_DIR ${OWNCLOUD_OSX_BUNDLE}/Contents/Resources/Translations)
    install(FILES ${client_I18N} DESTINATION ${QM_DIR})
    get_target_property(_qmake Qt5::qmake LOCATION)
    execute_process(COMMAND ${_qmake} -query QT_INSTALL_TRANSLATIONS
        OUTPUT_VARIABLE QT_TRANSLATIONS_DIR
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    file(GLOB qt_I18N ${QT_TRANSLATIONS_DIR}/qt_??.qm ${QT_TRANSLATIONS_DIR}/qt_??_??.qm)
    install(FILES ${qt_I18N} DESTINATION ${QM_DIR})
    file(GLOB qtbase_I18N ${QT_TRANSLATIONS_DIR}/qtbase_??.qm ${QT_TRANSLATIONS_DIR}/qt_??_??.qm)
    install(FILES ${qtbase_I18N} DESTINATION ${QM_DIR})
    file(GLOB qtkeychain_I18N ${QT_TRANSLATIONS_DIR}/qtkeychain*.qm)
    install(FILES ${qtkeychain_I18N} DESTINATION ${QM_DIR})
endif()

add_library(updater STATIC ${updater_SRCS})
target_link_libraries(updater ${synclib_NAME} Qt5::Widgets Qt5::Network Qt5::Xml Qt5::WebEngineWidgets)
target_include_directories(updater PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

set_target_properties( ${APPLICATION_EXECUTABLE} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY  ${BIN_OUTPUT_DIRECTORY}
)
# Only relevant for Linux? On OS X it by default properly checks in the bundle directory next to the exe
set_target_properties( ${APPLICATION_EXECUTABLE} PROPERTIES
        INSTALL_RPATH "${CMAKE_INSTALL_PREFIX}/${LIB_INSTALL_DIR}/${APPLICATION_EXECUTABLE};${CMAKE_INSTALL_RPATH}" )

target_link_libraries( ${APPLICATION_EXECUTABLE} Qt5::Widgets Qt5::Network Qt5::Xml)
target_link_libraries( ${APPLICATION_EXECUTABLE} ${synclib_NAME} )
target_link_libraries( ${APPLICATION_EXECUTABLE} updater )
target_link_libraries( ${APPLICATION_EXECUTABLE} ${OS_SPECIFIC_LINK_LIBRARIES} )
IF( LIBCLOUDPROVIDERS_FOUND )
    string(TOLOWER "${APPLICATION_VENDOR}" DBUS_VENDOR)
    string(REGEX REPLACE "[^A-z0-9]" "" DBUS_VENDOR "${DBUS_VENDOR}")
    string(REGEX REPLACE "[^A-z0-9]" "" DBUS_APPLICATION_NAME "${APPLICATION_SHORTNAME}")
    if(NOT DBUS_PREFIX)
        set(DBUS_PREFIX "com")
    endif(NOT DBUS_PREFIX)
    set(LIBCLOUDPROVIDERS_DBUS_BUS_NAME "${DBUS_PREFIX}.${DBUS_VENDOR}.${DBUS_APPLICATION_NAME}")
    set(LIBCLOUDPROVIDERS_DBUS_OBJECT_PATH "/${DBUS_PREFIX}/${DBUS_VENDOR}/${DBUS_APPLICATION_NAME}")

    configure_file(${CMAKE_CURRENT_SOURCE_DIR}/cloudproviders/cloudproviderconfig.h.in ${CMAKE_CURRENT_BINARY_DIR}/cloudproviderconfig.h)
    target_link_libraries( ${APPLICATION_EXECUTABLE} ${GLib2_LDFLAGS} ${GIO_LDFLAGS} ${LIBCLOUDPROVIDERS_LIBRARY} )
ENDIF()


target_include_directories(${APPLICATION_EXECUTABLE} PRIVATE
    ${CMAKE_SOURCE_DIR}/src/3rdparty/QProgressIndicator
    ${CMAKE_SOURCE_DIR}/src/3rdparty/qtlockedfile
    ${CMAKE_SOURCE_DIR}/src/3rdparty/qtmacgoodies/src
    ${CMAKE_SOURCE_DIR}/src/3rdparty/qtsingleapplication
    ${CMAKE_SOURCE_DIR}/src/3rdparty/kmessagewidget
    ${CMAKE_CURRENT_BINARY_DIR}
)

## handle DBUS for Fdo notifications
if( UNIX AND NOT APPLE )
    find_package(Qt5 COMPONENTS DBus)
    target_link_libraries( ${APPLICATION_EXECUTABLE} Qt5::DBus)
    target_compile_definitions(${APPLICATION_EXECUTABLE} PRIVATE "USE_FDO_NOTIFICATIONS")
endif()

if (APPLE)
    find_package(Qt5 COMPONENTS MacExtras)
    target_link_libraries( ${APPLICATION_EXECUTABLE} Qt5::MacExtras)
endif()

if (NOT NO_SHIBBOLETH)
    find_package(Qt5 COMPONENTS WebKitWidgets)
    target_link_libraries( ${APPLICATION_EXECUTABLE} Qt5::WebKitWidgets)
endif()

if(WITH_CRASHREPORTER)
    target_link_libraries(${APPLICATION_EXECUTABLE} crashreporter-handler)

    if(UNIX AND NOT MAC)
        find_package(Threads REQUIRED)
        target_link_libraries( ${APPLICATION_EXECUTABLE} ${CMAKE_THREAD_LIBS_INIT})
    endif()
endif()

# application.cpp still uses QDesktopServices::storageLocation
target_compile_definitions(${APPLICATION_EXECUTABLE} PRIVATE "QT_DISABLE_DEPRECATED_BEFORE=0")


install(TARGETS ${APPLICATION_EXECUTABLE}
        RUNTIME DESTINATION bin
        LIBRARY DESTINATION lib
        ARCHIVE DESTINATION lib
        BUNDLE  DESTINATION "."
 )


# FIXME: The following lines are dup in src/gui and src/cmd because it needs to be done after both are installed
#FIXME: find a nice solution to make the second if(BUILD_OWNCLOUD_OSX_BUNDLE) unnecessary
# currently it needs to be done because the code right above needs to be executed no matter
# if building a bundle or not and the install_qt4_executable needs to be called afterwards
if(BUILD_OWNCLOUD_OSX_BUNDLE AND NOT BUILD_LIBRARIES_ONLY)
   get_target_property (QT_QMAKE_EXECUTABLE Qt5::qmake IMPORTED_LOCATION)
   install(CODE "
    message(STATUS \"Deploying (Qt) dependencies and fixing library paths...\")
    execute_process(COMMAND \"${CMAKE_SOURCE_DIR}/admin/osx/macdeployqt.py\" ${CMAKE_INSTALL_PREFIX}/${OWNCLOUD_OSX_BUNDLE} ${QT_QMAKE_EXECUTABLE})
    execute_process(COMMAND \"${CMAKE_SOURCE_DIR}/admin/osx/macdeployqt.py\" ${CMAKE_INSTALL_PREFIX}/${OWNCLOUD_OSX_BUNDLE}/Contents/Frameworks/QtWebEngineCore.framework/Helpers/QtWebEngineProcess.app ${QT_QMAKE_EXECUTABLE})
    " COMPONENT RUNTIME)
endif()

if(NOT BUILD_OWNCLOUD_OSX_BUNDLE AND NOT WIN32)
    configure_file(${CMAKE_SOURCE_DIR}/mirall.desktop.in
                   ${CMAKE_CURRENT_BINARY_DIR}/${APPLICATION_EXECUTABLE}.desktop)
    install(FILES  ${CMAKE_CURRENT_BINARY_DIR}/${APPLICATION_EXECUTABLE}.desktop DESTINATION ${DATADIR}/applications )
endif()

