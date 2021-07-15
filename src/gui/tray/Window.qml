import QtQml 2.12
import QtQml.Models 2.1
import QtQuick 2.9
import QtQuick.Window 2.3
import QtQuick.Controls 2.3
import QtQuick.Layouts 1.2
import QtGraphicalEffects 1.0

// Custom qml modules are in /theme (and included by resources.qrc)
import Style 1.0

import com.nextcloud.desktopclient 1.0

Window {
    id:         trayWindow

    title:      Systray.windowTitle
    // If the main dialog is displayed as a regular window we want it to be quadratic
    width:      Systray.useNormalWindow ? Style.trayWindowHeight : Style.trayWindowWidth
    height:     Style.trayWindowHeight
    color:      "transparent"
    flags:      Qt.WindowTitleHint | Qt.CustomizeWindowHint | Qt.WindowCloseButtonHint | (Systray.useNormalWindow ? Qt.Dialog : Qt.Dialog | Qt.FramelessWindowHint)


    readonly property int maxMenuHeight: Style.trayWindowHeight - Style.trayWindowHeaderHeight - 2 * Style.trayWindowBorderWidth

    Component.onCompleted: Systray.forceWindowInit(trayWindow)

    // Close tray window when focus is lost (e.g. click somewhere else on the screen)
    onActiveChanged: {
        if (!Systray.useNormalWindow && !active) {
            hide();
            Systray.setClosed();
        }
   }

    onClosing: {
        Systray.setClosed()
    }

    onVisibleChanged: {
        // HACK: reload account Instantiator immediately by restting it - could be done better I guess
        // see also id:accountMenu below
        userLineInstantiator.active = false;
        userLineInstantiator.active = true;
    }

    Connections {
        target: UserModel
        onNewUserSelected: {
            accountMenu.close();
        }
    }

    Connections {
        target: Systray
        onShowWindow: {
            accountMenu.close();
            appsMenu.close();
            Systray.positionWindow(trayWindow);

            trayWindow.show();
            trayWindow.raise();
            trayWindow.requestActivate();

            Systray.setOpened();
            UserModel.fetchCurrentActivityModel();
        }
        onHideWindow: {
            trayWindow.hide();
            Systray.setClosed();
        }
    }

    OpacityMask {
        anchors.fill: parent
        source: ShaderEffectSource {
            sourceItem: trayWindowBackground
            hideSource: true
        }
        maskSource: Rectangle {
            width: trayWindowBackground.width
            height: trayWindowBackground.height
            radius: Systray.useNormalWindow ? 0.0 : Style.trayWindowRadius
        }
    }

    Rectangle {
        id: trayWindowBackground

        anchors.fill:   parent
        radius: Systray.useNormalWindow ? 0.0 : Style.trayWindowRadius
        border.width:   Style.trayWindowBorderWidth
        border.color:   Style.menuBorder

        Accessible.role: Accessible.Grouping
        Accessible.name: qsTr("Nextcloud desktop main dialog")

        Rectangle {
            id: trayWindowHeaderBackground

            anchors.left:   trayWindowBackground.left
            anchors.right:  trayWindowBackground.right
            anchors.top:    trayWindowBackground.top
            height:         Style.trayWindowHeaderHeight
            color:          Style.ncBlue

            RowLayout {
                id: trayWindowHeaderLayout

                spacing:        0
                anchors.fill:   parent

                Button {
                    id: currentAccountButton

                    Layout.preferredWidth:  Style.currentAccountButtonWidth
                    Layout.preferredHeight: Style.trayWindowHeaderHeight
                    display:                AbstractButton.IconOnly
                    flat:                   true

                    Accessible.role: Accessible.ButtonMenu
                    Accessible.name: qsTr("Current account")
                    Accessible.onPressAction: currentAccountButton.clicked()

                    MouseArea {
                        id: accountBtnMouseArea

                        anchors.fill:   parent
                        hoverEnabled:   Style.hoverEffectsEnabled

                        // We call open() instead of popup() because we want to position it
                        // exactly below the dropdown button, not the mouse
                        onClicked: {
                            syncPauseButton.text = Systray.syncIsPaused() ? qsTr("Resume sync for all") : qsTr("Pause sync for all")
                            if (accountMenu.visible) {
                                accountMenu.close()
                            } else {
                                accountMenu.open()
                            }
                        }

                        Menu {
                            id: accountMenu

                            // x coordinate grows towards the right
                            // y coordinate grows towards the bottom
                            x: (currentAccountButton.x + 2)
                            y: (currentAccountButton.y + Style.trayWindowHeaderHeight + 2)

                            width: (Style.currentAccountButtonWidth - 2)
                            height: Math.min(implicitHeight, maxMenuHeight)
                            closePolicy: Menu.CloseOnPressOutsideParent | Menu.CloseOnEscape

                            background: Rectangle {
                                border.color: Style.menuBorder
                                radius: Style.currentAccountButtonRadius
                            }

                            onClosed: {
                                // HACK: reload account Instantiator immediately by restting it - could be done better I guess
                                // see also onVisibleChanged above
                                userLineInstantiator.active = false;
                                userLineInstantiator.active = true;
                            }

                            Instantiator {
                                id: userLineInstantiator
                                model: UserModel
                                delegate: UserLine {}
                                onObjectAdded: accountMenu.insertItem(index, object)
                                onObjectRemoved: accountMenu.removeItem(object)
                            }

                            MenuItem {
                                id: addAccountButton
                                height: Style.addAccountButtonHeight
                                hoverEnabled: true

                                background: Item {
                                    height: parent.height
                                    width: parent.menu.width
                                    Rectangle {
                                        anchors.fill: parent
                                        anchors.margins: 1
                                        color: parent.parent.hovered ? Style.lightHover : "transparent"
                                    }
                                }

                                RowLayout {
                                    anchors.fill: parent
                                    spacing: 0

                                    Image {
                                        Layout.leftMargin: 12
                                        verticalAlignment: Qt.AlignCenter
                                        source: "qrc:///client/theme/black/add.svg"
                                        sourceSize.width: Style.headerButtonIconSize
                                        sourceSize.height: Style.headerButtonIconSize
                                    }
                                    Label {
                                        Layout.leftMargin: 14
                                        text: qsTr("Add account")
                                        color: "black"
                                        font.pixelSize: Style.topLinePixelSize
                                    }
                                    // Filler on the right
                                    Item {
                                        Layout.fillWidth: true
                                        Layout.fillHeight: true
                                    }
                                }
                                onClicked: UserModel.addAccount()

                                Accessible.role: Accessible.MenuItem
                                Accessible.name: qsTr("Add new account")
                                Accessible.onPressAction: addAccountButton.clicked()
                            }

                            MenuSeparator {
                                contentItem: Rectangle {
                                    implicitHeight: 1
                                    color: Style.menuBorder
                                }
                            }

                            MenuItem {
                                id: syncPauseButton
                                font.pixelSize: Style.topLinePixelSize
                                hoverEnabled: true
                                onClicked: Systray.pauseResumeSync()

                                background: Item {
                                    height: parent.height
                                    width: parent.menu.width
                                    Rectangle {
                                        anchors.fill: parent
                                        anchors.margins: 1
                                        color: parent.parent.hovered ? Style.lightHover : "transparent"
                                    }
                                }

                                Accessible.role: Accessible.MenuItem
                                Accessible.name: Systray.syncIsPaused() ? qsTr("Resume sync for all") : qsTr("Pause sync for all")
                                Accessible.onPressAction: syncPauseButton.clicked()
                            }

                            MenuItem {
                                id: settingsButton
                                text: qsTr("Settings")
                                font.pixelSize: Style.topLinePixelSize
                                hoverEnabled: true
                                onClicked: Systray.openSettings()

                                background: Item {
                                    height: parent.height
                                    width: parent.menu.width
                                    Rectangle {
                                        anchors.fill: parent
                                        anchors.margins: 1
                                        color: parent.parent.hovered ? Style.lightHover : "transparent"
                                    }
                                }

                                Accessible.role: Accessible.MenuItem
                                Accessible.name: text
                                Accessible.onPressAction: settingsButton.clicked()
                            }

                            MenuItem {
                                id: exitButton
                                text: qsTr("Exit");
                                font.pixelSize: Style.topLinePixelSize
                                hoverEnabled: true
                                onClicked: Systray.shutdown()

                                background: Item {
                                    height: parent.height
                                    width: parent.menu.width
                                    Rectangle {
                                        anchors.fill: parent
                                        anchors.margins: 1
                                        color: parent.parent.hovered ? Style.lightHover : "transparent"
                                    }
                                }

                                Accessible.role: Accessible.MenuItem
                                Accessible.name: text
                                Accessible.onPressAction: exitButton.clicked()
                            }
                        }
                    }

                    background: Rectangle {
                        color: accountBtnMouseArea.containsMouse ? "white" : "transparent"
                        opacity: 0.2
                    }

                    RowLayout {
                        id: accountControlRowLayout

                        height: Style.trayWindowHeaderHeight
                        width:  Style.currentAccountButtonWidth
                        spacing: 0

                        Image {
                            id: currentAccountAvatar

                            Layout.leftMargin: 8
                            verticalAlignment: Qt.AlignCenter
                            cache: false
                            source: UserModel.currentUser.avatar != "" ? UserModel.currentUser.avatar : "image://avatars/fallbackWhite"
                            Layout.preferredHeight: Style.accountAvatarSize
                            Layout.preferredWidth: Style.accountAvatarSize

                            Accessible.role: Accessible.Graphic
                            Accessible.name: qsTr("Current user avatar")

                            Rectangle {
                                id: currentAccountStatusIndicatorBackground
                                visible: UserModel.currentUser.isConnected
                                         && UserModel.currentUser.serverHasUserStatus
                                width: Style.accountAvatarStateIndicatorSize + 2
                                height: width
                                anchors.bottom: currentAccountAvatar.bottom
                                anchors.right: currentAccountAvatar.right
                                color: Style.ncBlue
                                radius: width*0.5
                            }

                            Rectangle {
                                id: currentAccountStatusIndicatorMouseHover
                                visible: UserModel.currentUser.isConnected
                                         && UserModel.currentUser.serverHasUserStatus
                                width: Style.accountAvatarStateIndicatorSize + 2
                                height: width
                                anchors.bottom: currentAccountAvatar.bottom
                                anchors.right: currentAccountAvatar.right
                                color: accountBtnMouseArea.containsMouse ? "white" : "transparent"
                                opacity: 0.2
                                radius: width*0.5
                            }

                            Image {
                                id: currentAccountStatusIndicator
                                visible: UserModel.currentUser.isConnected
                                         && UserModel.currentUser.serverHasUserStatus
                                source: UserModel.currentUser.statusIcon
                                cache: false
                                x: currentAccountStatusIndicatorBackground.x + 1
                                y: currentAccountStatusIndicatorBackground.y + 1
                                sourceSize.width: Style.accountAvatarStateIndicatorSize
                                sourceSize.height: Style.accountAvatarStateIndicatorSize

                                Accessible.role: Accessible.Indicator
                                Accessible.name: UserModel.desktopNotificationsAllowed ? qsTr("Current user status is online") : qsTr("Current user status is do not disturb")
                            }
                        }

                        Column {
                            id: accountLabels
                            spacing: 0
                            Layout.alignment: Qt.AlignLeft | Qt.AlignVCenter
                            Layout.leftMargin: Style.userStatusSpacing
                            Label {
                                id: currentAccountUser
                                Layout.alignment: Qt.AlignLeft | Qt.AlignBottom
                                width: Style.currentAccountLabelWidth
                                text: UserModel.currentUser.name
                                elide: Text.ElideRight
                                color: Style.ncTextColor
                                font.pixelSize: Style.topLinePixelSize
                                font.bold: true
                            }
                            RowLayout {
                                id: currentUserStatus
                                visible: UserModel.currentUser.isConnected &&
                                         UserModel.currentUser.serverHasUserStatus
                                spacing: Style.accountLabelsSpacing
                                Label {
                                    id: emoji
                                    visible: UserModel.currentUser.statusEmoji !== ""
                                    width: Style.userStatusEmojiSize
                                    text: UserModel.currentUser.statusEmoji
                                }
                                Label {
                                    id: message
                                    Layout.alignment: Qt.AlignLeft | Qt.AlignBottom
                                    visible: UserModel.currentUser.statusMessage !== ""
                                    width: Style.currentAccountLabelWidth
                                    text: UserModel.currentUser.statusMessage !== ""
                                          ? UserModel.currentUser.statusMessage 
                                          : UserModel.currentUser.server
                                    elide: Text.ElideRight
                                    color: Style.ncTextColor
                                    font.pixelSize: Style.subLinePixelSize
                                }
                            }
                        }

                        ColorOverlay {
                            cached: true
                            color: Style.ncTextColor
                            width: source.width
                            height: source.height
                            source: Image {
                                Layout.alignment: Qt.AlignRight
                                verticalAlignment: Qt.AlignCenter
                                Layout.margins: Style.accountDropDownCaretMargin
                                source: "qrc:///client/theme/white/caret-down.svg"
                                sourceSize.width: Style.accountDropDownCaretSize
                                sourceSize.height: Style.accountDropDownCaretSize
                                Accessible.role: Accessible.PopupMenu
                                Accessible.name: qsTr("Account switcher and settings menu")
                            }
                        }
                    }
                }

                // Add space between items
                Item {
                    Layout.fillWidth: true
                }
                
                RowLayout {
                    id: openLocalFolderRowLayout
                    spacing: 0
                    Layout.preferredWidth:  Style.trayWindowHeaderHeight
                    Layout.preferredHeight: Style.trayWindowHeaderHeight
                    Layout.alignment: Qt.AlignHCenter | Qt.AlignVCenter
                    
                    HeaderButton {
                        id: openLocalFolderButton
                        visible: UserModel.currentUser.hasLocalFolder
                        icon.source: "qrc:///client/theme/white/folder.svg"
                        onClicked: UserModel.openCurrentAccountLocalFolder()
                        
                        Rectangle {
                            id: folderStateIndicatorBackground
                            width: Style.folderStateIndicatorSize
                            height: width
                            anchors.top: openLocalFolderButton.verticalCenter
                            anchors.left: openLocalFolderButton.horizontalCenter
                            color: Style.ncBlue
                            radius: width*0.5
                            z: 1
                        }
    
                        Image {
                            id: folderStateIndicator
                            visible: UserModel.currentUser.hasLocalFolder
                            source: UserModel.currentUser.isConnected
                                    ? Style.stateOnlineImageSource
                                    : Style.stateOfflineImageSource
                            cache: false
                            
                            anchors.top: openLocalFolderButton.verticalCenter
                            anchors.left: openLocalFolderButton.horizontalCenter  
                            sourceSize.width: Style.folderStateIndicatorSize
                            sourceSize.height: Style.folderStateIndicatorSize
        
                            Accessible.role: Accessible.Indicator
                            Accessible.name: UserModel.currentUser.isConnected ? qsTr("Connected") : qsTr("Disconnected")
                            z: 2
                        }
                    }
                    
 

                    Accessible.role: Accessible.Button
                    Accessible.name: qsTr("Open local folder of current account")
                }

                HeaderButton {
                    id: trayWindowTalkButton
                    
                    visible: UserModel.currentUser.serverHasTalk
                    icon.source: "qrc:///client/theme/white/talk-app.svg"
                    onClicked: UserModel.openCurrentAccountTalk()
                    
                    Accessible.role: Accessible.Button
                    Accessible.name: qsTr("Open Nextcloud Talk in browser")
                    Accessible.onPressAction: trayWindowTalkButton.clicked()
                }

                HeaderButton {
                    id: trayWindowAppsButton
                    icon.source: "qrc:///client/theme/white/more-apps.svg"
  
                    onClicked: {
                        if(appsMenu.count <= 0) {
                            UserModel.openCurrentAccountServer()
                        } else if (appsMenu.visible) {
                            appsMenu.close()
                        } else {
                            appsMenu.open()
                        }
                    }

                    Accessible.role: Accessible.ButtonMenu
                    Accessible.name: qsTr("More apps")
                    Accessible.onPressAction: trayWindowAppsButton.clicked()

                    Menu {
                        id: appsMenu
                        y: (trayWindowAppsButton.y + trayWindowAppsButton.height + 2)
                        readonly property Item listContentItem: contentItem.contentItem
                        width: Math.min(listContentItem.childrenRect.width + 4, Style.trayWindowWidth / 2)
                        height: Math.min(implicitHeight, maxMenuHeight)
                        closePolicy: Menu.CloseOnPressOutsideParent | Menu.CloseOnEscape

                        background: Rectangle {
                            border.color: Style.menuBorder
                            radius: 2
                        }

                        Instantiator {
                            id: appsMenuInstantiator
                            model: UserAppsModel
                            onObjectAdded: appsMenu.insertItem(index, object)
                            onObjectRemoved: appsMenu.removeItem(object)
                            delegate: MenuItem {
                                id: appEntry
                                text: appName
                                font.pixelSize: Style.topLinePixelSize
                                icon.source: appIconUrl
                                width: contentItem.implicitWidth + leftPadding + rightPadding
                                onTriggered: UserAppsModel.openAppUrl(appUrl)
                                hoverEnabled: true

                                background: Item {
                                    width: appsMenu.width
                                    height: parent.height

                                    Rectangle {
                                        anchors.fill: parent
                                        anchors.margins: 1
                                        color: appEntry.hovered ? Style.lightHover : "transparent"
                                    }
                                    
                                    Accessible.role: Accessible.PopupMenu
                                    Accessible.name: qsTr("Apps menu")
                                }

                                Accessible.role: Accessible.MenuItem
                                Accessible.name: qsTr("Open %1 in browser").arg(appName)
                                Accessible.onPressAction: appEntry.triggered()
                            }
                        }
                    }
                }
            }
        }   // Rectangle trayWindowHeaderBackground

        ListView {
            id: activityListView
            anchors.top: trayWindowHeaderBackground.bottom
            anchors.left: trayWindowBackground.left
            anchors.right: trayWindowBackground.right
            anchors.bottom: trayWindowBackground.bottom
            clip: true
            ScrollBar.vertical: ScrollBar {
                id: listViewScrollbar
            }

            readonly property int maxActionButtons: 2

            keyNavigationEnabled: true

            Accessible.role: Accessible.List
            Accessible.name: qsTr("Activity list")

            model: activityModel

            delegate: ActivityItem {  
            }
        }
    }       // Rectangle trayWindowBackground
}
