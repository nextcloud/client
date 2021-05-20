project(gui)
find_package(Qt5 REQUIRED COMPONENTS Widgets Svg Qml Quick QuickControls2)

if(CMAKE_BUILD_TYPE MATCHES Debug)
    add_definitions(-DQT_QML_DEBUG)
endif()

IF(BUILD_UPDATER)
    add_subdirectory(updater)
endif()

configure_file(${CMAKE_SOURCE_DIR}/theme.qrc.in ${CMAKE_SOURCE_DIR}/theme.qrc)

set(MIRALL_RC_SRC ../../resources.qrc)
list(APPEND MIRALL_RC_SRC ${CMAKE_SOURCE_DIR}/theme.qrc)
set(theme_dir ${CMAKE_SOURCE_DIR}/theme)

set(client_UI_SRCS
    accountsettings.ui
    conflictdialog.ui
    foldercreationdialog.ui
    folderwizardsourcepage.ui
    folderwizardtargetpage.ui
    generalsettings.ui
    legalnotice.ui
    ignorelisteditor.ui
    ignorelisttablewidget.ui
    networksettings.ui
    settingsdialog.ui
    sharedialog.ui
    sharelinkwidget.ui
    shareusergroupwidget.ui
    shareuserline.ui
    sslerrordialog.ui
    addcertificatedialog.ui
    proxyauthdialog.ui
    mnemonicdialog.ui
    tray/ActivityActionButton.qml
    tray/Window.qml
    tray/UserLine.qml
    wizard/flow2authwidget.ui
    wizard/owncloudadvancedsetuppage.ui
    wizard/owncloudconnectionmethoddialog.ui
    wizard/owncloudhttpcredspage.ui
    wizard/owncloudoauthcredspage.ui
    wizard/owncloudsetupnocredspage.ui
    wizard/owncloudwizardresultpage.ui
    wizard/webview.ui
    wizard/welcomepage.ui
)

set(client_SRCS
    accountmanager.cpp
    accountsettings.cpp
    application.cpp
    conflictdialog.cpp
    conflictsolver.cpp
    connectionvalidator.cpp
    folder.cpp
    foldercreationdialog.cpp
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
    networksettings.cpp
    ocsnavigationappsjob.cpp
    ocsjob.cpp
    ocssharejob.cpp
    ocsshareejob.cpp
    openfilemanager.cpp
    owncloudgui.cpp
    owncloudsetupwizard.cpp
    selectivesyncdialog.cpp
    settingsdialog.cpp
    sharedialog.cpp
    activityjob.cpp
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
    userinfo.cpp
    userstatus.cpp
    accountstate.cpp
    addcertificatedialog.cpp
    authenticationdialog.cpp
    proxyauthhandler.cpp
    proxyauthdialog.cpp
    tooltipupdater.cpp
    notificationconfirmjob.cpp
    guiutility.cpp
    elidedlabel.cpp
    headerbanner.cpp
    iconjob.cpp
    remotewipe.cpp
    tray/ActivityData.cpp
    tray/ActivityListModel.cpp
    tray/UserModel.cpp
    tray/NotificationHandler.cpp
    tray/NotificationCache.cpp
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
    wizard/welcomepage.cpp
    wizard/linklabel.cpp
)

IF(BUILD_UPDATER)
    set(updater_SRCS
        updater/ocupdater.cpp
        updater/updateinfo.cpp
        updater/updater.cpp
    )
endif()

IF( APPLE )
    list(APPEND client_SRCS cocoainitializer_mac.mm)
    list(APPEND client_SRCS socketapisocket_mac.mm)
    list(APPEND client_SRCS systray.mm)

    if(SPARKLE_FOUND AND BUILD_UPDATER)
        # Define this, we need to check in updater.cpp
        add_definitions(-DHAVE_SPARKLE)
        list(APPEND updater_SRCS updater/sparkleupdater_mac.mm updater/sparkleupdater.h)
        list(APPEND updater_DEPS ${SPARKLE_LIBRARY})

        # Sparkle.framework is installed from here because macdeployqt's CopyFramework breaks on this bundle
        # as its logic is tightly tailored around Qt5 frameworks
        install(DIRECTORY "${SPARKLE_LIBRARY}"
                DESTINATION "${OWNCLOUD_OSX_BUNDLE}/Contents/Frameworks" USE_SOURCE_PERMISSIONS)

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

if(NOT WIN32)
   list(APPEND 3rdparty_SRC ../3rdparty/qtlockedfile/qtlockedfile_unix.cpp)
else()
   list(APPEND 3rdparty_SRC ../3rdparty/qtlockedfile/qtlockedfile_win.cpp )
endif()

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
    ${guiMoc}
    ${client_I18N}
    ${3rdparty_SRC}
    ${3rdparty_MOC}
)

if(Qt5Keychain_FOUND)
    list(APPEND libsync_LINK_TARGETS qt5keychain)
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

# Generate png icons from svg
find_program(INKSCAPE
  NAMES inkscape inkscape.exe
  REQUIRED
  HINTS "C:\\Program Files\\Inkscape\\bin" "/usr/bin" ENV INKSCAPE_DIR)
# REQUIRED keyword is only supported on CMake 3.18 and above
if (NOT INKSCAPE)
  message(FATAL_ERROR "Could not find inkscape. Set INKSCAPE_DIR to the path of executable.")
endif()

function(generate_sized_png_from_svg icon_path size)
  get_filename_component(icon_name_dir ${icon_path} DIRECTORY)
  get_filename_component(icon_name_wle ${icon_path} NAME_WLE)

  if (EXISTS "${icon_name_dir}/${size}-${icon_name_wle}.png")
    return()
  endif()

  set(icon_output_name "${size}-${icon_name_wle}.png")
  message(STATUS "Generate ${icon_output_name}")
  execute_process(COMMAND
    "${INKSCAPE}" -w ${size} -h ${size} "${icon_path}" -o "${icon_output_name}"
    WORKING_DIRECTORY "${icon_name_dir}"
    RESULT_VARIABLE
    INKSCAPE_SIDEBAR_ERROR
    OUTPUT_QUIET
    ERROR_QUIET)

  if (INKSCAPE_SIDEBAR_ERROR)
    message(FATAL_ERROR
      "inkscape could not generate icon: ${INKSCAPE_SIDEBAR_ERROR}")
  else()
  endif()
endfunction()

if ((APPLICATION_ICON_SET MATCHES "PNG")
    AND
    (NOT EXISTS "${theme_dir}/colored/${APPLICATION_ICON_NAME}-icon.svg"))
  # We may have no svg application icon in when customers use PNG
  # icons in brander, but theme.qrc expects a svg icon.
  file(TOUCH "${theme_dir}/colored/${APPLICATION_ICON_NAME}-icon.svg")
endif()

if(APPLE)
  set(MACOS_SIDEBAR_ICON_SVG "${theme_dir}/colored/${APPLICATION_ICON_NAME}-sidebar.svg")
  generate_sized_png_from_svg(${MACOS_SIDEBAR_ICON_SVG} 16)
  generate_sized_png_from_svg(${MACOS_SIDEBAR_ICON_SVG} 32)
  generate_sized_png_from_svg(${MACOS_SIDEBAR_ICON_SVG} 64)
  generate_sized_png_from_svg(${MACOS_SIDEBAR_ICON_SVG} 128)
  generate_sized_png_from_svg(${MACOS_SIDEBAR_ICON_SVG} 256)
endif()

if(WIN32)
  set(STARTMENU_ICON_SVG "${theme_dir}/colored/${APPLICATION_ICON_NAME}-w10startmenu.svg")
  generate_sized_png_from_svg(${STARTMENU_ICON_SVG} 70)
  generate_sized_png_from_svg(${STARTMENU_ICON_SVG} 150)
endif()

set(APP_ICON_SVG "${theme_dir}/colored/${APPLICATION_ICON_NAME}-icon.svg")
generate_sized_png_from_svg(${APP_ICON_SVG} 16)
generate_sized_png_from_svg(${APP_ICON_SVG} 24)
generate_sized_png_from_svg(${APP_ICON_SVG} 32)
generate_sized_png_from_svg(${APP_ICON_SVG} 48)
generate_sized_png_from_svg(${APP_ICON_SVG} 64)
generate_sized_png_from_svg(${APP_ICON_SVG} 128)
generate_sized_png_from_svg(${APP_ICON_SVG} 256)
generate_sized_png_from_svg(${APP_ICON_SVG} 512)
generate_sized_png_from_svg(${APP_ICON_SVG} 1024)

file(GLOB_RECURSE OWNCLOUD_ICONS "${theme_dir}/colored/*-${APPLICATION_ICON_NAME}-icon*")
if(APPLE)
    file(GLOB_RECURSE OWNCLOUD_SIDEBAR_ICONS "${theme_dir}/colored/*-${APPLICATION_ICON_NAME}-sidebar*")
    MESSAGE(STATUS "OWNCLOUD_SIDEBAR_ICONS: ${APPLICATION_ICON_NAME}: ${OWNCLOUD_SIDEBAR_ICONS}")
endif()
ecm_add_app_icon(APP_ICON ICONS "${OWNCLOUD_ICONS}" SIDEBAR_ICONS "${OWNCLOUD_SIDEBAR_ICONS}" OUTFILE_BASENAME "${APPLICATION_ICON_NAME}")

if(UNIX AND NOT APPLE)
  set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fPIE")
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fPIE")
  set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -pie")
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "FreeBSD")
  set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -L/usr/local/lib")
endif()

add_library(nextcloudCore STATIC ${final_src})

target_link_libraries(nextcloudCore
  PUBLIC
  Qt5::Widgets
  Qt5::GuiPrivate
  Qt5::Svg
  Qt5::Network
  Qt5::Xml
  Qt5::Qml
  Qt5::Quick
  Qt5::QuickControls2
  Qt5::WebEngineWidgets
  ${synclib_NAME}
)

set_target_properties(nextcloudCore
  PROPERTIES
  AUTOUIC ON
  AUTORCC ON
  AUTOMOC ON
)

target_include_directories(nextcloudCore
  PUBLIC
  ${CMAKE_SOURCE_DIR}/src/3rdparty/QProgressIndicator
  ${CMAKE_SOURCE_DIR}/src/3rdparty/qtlockedfile
  ${CMAKE_SOURCE_DIR}/src/3rdparty/qtsingleapplication
  ${CMAKE_SOURCE_DIR}/src/3rdparty/kmessagewidget
  ${CMAKE_CURRENT_BINARY_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}
)

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
    add_executable(${APPLICATION_EXECUTABLE} WIN32 main.cpp ${client_version} ${client_manifest} ${APP_ICON})
else()
    # set(CMAKE_INSTALL_PREFIX ".") # Examples use /Applications. hurmpf.
    set(MACOSX_BUNDLE_ICON_FILE "${APPLICATION_ICON_NAME}.icns")

    # we must add MACOSX_BUNDLE only if building a bundle
    add_executable(${APPLICATION_EXECUTABLE} WIN32 MACOSX_BUNDLE main.cpp ${APP_ICON})

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

IF(BUILD_UPDATER)
    add_library(updater STATIC ${updater_SRCS})
    target_link_libraries(updater ${synclib_NAME} ${updater_DEPS} Qt5::Widgets Qt5::Svg Qt5::Network Qt5::Xml)
    target_include_directories(updater PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
    set_target_properties(updater PROPERTIES AUTOMOC ON)
endif()

set_target_properties( ${APPLICATION_EXECUTABLE} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY  ${BIN_OUTPUT_DIRECTORY}
)

target_link_libraries(${APPLICATION_EXECUTABLE} nextcloudCore)

IF(BUILD_UPDATER)
  target_link_libraries(nextcloudCore PUBLIC updater)
endif()

target_link_libraries(nextcloudCore PUBLIC ${OS_SPECIFIC_LINK_LIBRARIES})

if(TARGET PkgConfig::CLOUDPROVIDERS)
    message("Building with libcloudproviderssupport")
    target_sources(nextcloudCore PRIVATE cloudproviders/cloudprovidermanager.cpp cloudproviders/cloudproviderwrapper.cpp)

    string(TOLOWER "${APPLICATION_VENDOR}" DBUS_VENDOR)
    string(REGEX REPLACE "[^A-z0-9]" "" DBUS_VENDOR "${DBUS_VENDOR}")
    string(REGEX REPLACE "[^A-z0-9]" "" DBUS_APPLICATION_NAME "${APPLICATION_SHORTNAME}")
    if(NOT DBUS_PREFIX)
        set(DBUS_PREFIX "com")
    endif(NOT DBUS_PREFIX)
    set(LIBCLOUDPROVIDERS_DBUS_BUS_NAME "${DBUS_PREFIX}.${DBUS_VENDOR}.${DBUS_APPLICATION_NAME}")
    set(LIBCLOUDPROVIDERS_DBUS_OBJECT_PATH "/${DBUS_PREFIX}/${DBUS_VENDOR}/${DBUS_APPLICATION_NAME}")

    configure_file(${CMAKE_CURRENT_SOURCE_DIR}/cloudproviders/cloudproviderconfig.h.in ${CMAKE_CURRENT_BINARY_DIR}/cloudproviderconfig.h)
    target_compile_definitions(nextcloudCore PUBLIC -DWITH_LIBCLOUDPROVIDERS)
    target_link_libraries(nextcloudCore
      PRIVATE
      PkgConfig::CLOUDPROVIDERS
      PkgConfig::GLIB2
      PkgConfig::GIO
    )
endif()

## handle DBUS for Fdo notifications
if( UNIX AND NOT APPLE )
    find_package(Qt5 COMPONENTS DBus)
    target_link_libraries(nextcloudCore PUBLIC Qt5::DBus)
    target_compile_definitions(nextcloudCore PUBLIC "USE_FDO_NOTIFICATIONS")
endif()

if (APPLE)
    find_package(Qt5 COMPONENTS MacExtras)
    target_link_libraries(nextcloudCore PUBLIC Qt5::MacExtras)
endif()

if(WITH_CRASHREPORTER)
    target_link_libraries(nextcloudCore PUBLIC crashreporter-handler)

    if(UNIX AND NOT MAC)
        find_package(Threads REQUIRED)
        target_link_libraries(nextcloudCore PUBLIC Threads::Threads)
    endif()
endif()

# application.cpp still uses QDesktopServices::storageLocation
target_compile_definitions(nextcloudCore PRIVATE "QT_DISABLE_DEPRECATED_BEFORE=0")


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
#
# OSX: Run macdeployqt for src/gui and for src/cmd using the -executable option
if(BUILD_OWNCLOUD_OSX_BUNDLE AND NOT BUILD_LIBRARIES_ONLY)
    get_target_property (QT_QMAKE_EXECUTABLE Qt5::qmake IMPORTED_LOCATION)
    get_filename_component(QT_BIN_DIR "${QT_QMAKE_EXECUTABLE}" DIRECTORY)
    find_program(MACDEPLOYQT_EXECUTABLE macdeployqt HINTS "${QT_BIN_DIR}")

    set(cmd_NAME ${APPLICATION_EXECUTABLE}cmd)

    if(CMAKE_BUILD_TYPE MATCHES Debug)
        set(NO_STRIP "-no-strip")
    else()
        set(NO_STRIP "")
    endif()

    add_custom_command(TARGET ${APPLICATION_EXECUTABLE} POST_BUILD
        COMMAND "${MACDEPLOYQT_EXECUTABLE}"
        "$<TARGET_FILE_DIR:${APPLICATION_EXECUTABLE}>/../.."
        -qmldir=${CMAKE_SOURCE_DIR}/src/gui
        -always-overwrite
        -executable="$<TARGET_FILE_DIR:${APPLICATION_EXECUTABLE}>/${cmd_NAME}"
        ${NO_STRIP}
        COMMAND "${CMAKE_COMMAND}"
        -E rm -rf "${BIN_OUTPUT_DIRECTORY}/${OWNCLOUD_OSX_BUNDLE}/Contents/PlugIns/bearer"
        COMMENT "Running macdeployqt..."
    )
endif()

if(NOT BUILD_OWNCLOUD_OSX_BUNDLE AND NOT WIN32)
    configure_file(${CMAKE_SOURCE_DIR}/mirall.desktop.in
                   ${CMAKE_CURRENT_BINARY_DIR}/${LINUX_APPLICATION_ID}.desktop)
    install(FILES  ${CMAKE_CURRENT_BINARY_DIR}/${LINUX_APPLICATION_ID}.desktop DESTINATION ${DATADIR}/applications )

    configure_file(owncloud.xml.in ${APPLICATION_EXECUTABLE}.xml)
    install(FILES ${CMAKE_CURRENT_BINARY_DIR}/${APPLICATION_EXECUTABLE}.xml DESTINATION ${DATADIR}/mime/packages )

    find_package(SharedMimeInfo)
    if(SharedMimeInfo_FOUND)
        update_xdg_mimetypes( ${DATADIR}/mime/packages )
    endif(SharedMimeInfo_FOUND)
endif()
