import QtQml 2.1
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

    width:      Style.trayWindowWidth
    height:     Style.trayWindowHeight
    color:      "transparent"
    flags:      Qt.Dialog | Qt.FramelessWindowHint

    readonly property int maxMenuHeight: Style.trayWindowHeight - Style.trayWindowHeaderHeight - 2 * Style.trayWindowBorderWidth

    // Close tray window when focus is lost (e.g. click somewhere else on the screen)
    onActiveChanged: {
        if(!active) {
            trayWindow.hide();
            Systray.setClosed();
        }
    }

    onVisibleChanged: {
        currentAccountAvatar.source = ""
        currentAccountAvatar.source = "image://avatars/currentUser"
        currentAccountStateIndicator.source = ""
        currentAccountStateIndicator.source = UserModel.isUserConnected(UserModel.currentUserId()) ? "qrc:///client/theme/colored/state-ok.svg" : "qrc:///client/theme/colored/state-offline.svg"

        // HACK: reload account Instantiator immediately by restting it - could be done better I guess
        // see also id:accountMenu below
        userLineInstantiator.active = false;
        userLineInstantiator.active = true;
    }

    Connections {
        target: notificationLayer
        ignoreUnknownSignals: true
        onDismissNotification: {
            Systray.dismissLastNotification()
        }
    }

    Connections {
        target: UserModel
        onRefreshCurrentUserGui: {
            currentAccountAvatar.source = ""
            currentAccountAvatar.source = "image://avatars/currentUser"
            currentAccountStateIndicator.source = ""
            currentAccountStateIndicator.source = UserModel.isUserConnected(UserModel.currentUserId()) ? "qrc:///client/theme/colored/state-ok.svg" : "qrc:///client/theme/colored/state-offline.svg"
        }
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
        onNewNotification: {
            notificationLayer.text = Systray.getLastNotification();
            notificationLayer.visible = true;
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
            radius: trayWindowBackground.radius
        }
    }

    Rectangle {
        id: trayWindowBackground

        anchors.fill:   parent
        radius:         Style.trayWindowRadius
        border.width:   Style.trayWindowBorderWidth
        border.color:   Style.menuBorder

        Rectangle {
            id: trayWindowHeaderBackground

            anchors.left:   trayWindowBackground.left
            anchors.top:    trayWindowBackground.top
            height:         Style.trayWindowHeaderHeight
            width:          Style.trayWindowWidth
            color:          Style.ncBlue

            // The overlay rectangle below eliminates the rounded corners from the bottom of the header
            // as Qt only allows setting the radius for all corners right now, not specific ones
            Rectangle {
                id: trayWindowHeaderButtomHalfBackground

                anchors.left:   trayWindowHeaderBackground.left
                anchors.bottom: trayWindowHeaderBackground.bottom
                height:         Style.trayWindowHeaderHeight / 2
                width:          Style.trayWindowWidth
                color:          Style.ncBlue
            }

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

                    MouseArea {
                        id: accountBtnMouseArea

                        anchors.fill:   parent
                        hoverEnabled:   Style.hoverEffectsEnabled

                        // HACK: Imitate Qt hover effect brightness (which is not accessible as property)
                        // so that indicator background also flicks when hovered
                        onContainsMouseChanged: {
                            currentAccountStateIndicatorBackground.color = (containsMouse ? Style.ncBlueHover : Style.ncBlue)
                        }

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
                            }

                            MenuItem {
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
                            }

                            MenuItem {
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
                            source: "image://avatars/currentUser"
                            Layout.preferredHeight: Style.accountAvatarSize
                            Layout.preferredWidth: Style.accountAvatarSize

                            Rectangle {
                                id: currentAccountStateIndicatorBackground
                                width: Style.accountAvatarStateIndicatorSize + 2
                                height: width
                                anchors.bottom: currentAccountAvatar.bottom
                                anchors.right: currentAccountAvatar.right
                                color: Style.ncBlue
                                radius: width*0.5
                            }

                            Image {
                                id: currentAccountStateIndicator
                                source: UserModel.isUserConnected(UserModel.currentUserId()) ? "qrc:///client/theme/colored/state-ok.svg" : "qrc:///client/theme/colored/state-offline.svg"
                                cache: false
                                x: currentAccountStateIndicatorBackground.x + 1
                                y: currentAccountStateIndicatorBackground.y + 1
                                sourceSize.width: Style.accountAvatarStateIndicatorSize
                                sourceSize.height: Style.accountAvatarStateIndicatorSize
                            }
                        }

                        Column {
                            id: accountLabels
                            spacing: 4
                            Layout.alignment: Qt.AlignLeft
                            Layout.leftMargin: 6
                            Label {
                                id: currentAccountUser

                                width: Style.currentAccountLabelWidth
                                text: UserModel.currentUser.name
                                elide: Text.ElideRight
                                color: "white"
                                font.pixelSize: Style.topLinePixelSize
                                font.bold: true
                            }
                            Label {
                                id: currentAccountServer
                                width: Style.currentAccountLabelWidth
                                text: UserModel.currentUser.server
                                elide: Text.ElideRight
                                color: "white"
                                font.pixelSize: Style.subLinePixelSize
                            }
                        }

                        Image {
                            Layout.alignment: Qt.AlignRight
                            verticalAlignment: Qt.AlignCenter
                            Layout.margins: Style.accountDropDownCaretMargin
                            source: "qrc:///client/theme/white/caret-down.svg"
                            sourceSize.width: Style.accountDropDownCaretSize
                            sourceSize.height: Style.accountDropDownCaretSize
                        }
                    }
                }

                // Filler between account dropdown and header app buttons
                Item {
                    id: trayWindowHeaderSpacer
                    Layout.fillWidth: true
                }

                HeaderButton {
                    id: openLocalFolderButton

                    visible: UserModel.currentUser.hasLocalFolder
                    icon.source: "qrc:///client/theme/white/folder.svg"
                    onClicked: UserModel.openCurrentAccountLocalFolder()
                }

                HeaderButton {
                    id: trayWindowTalkButton

                    visible: UserModel.currentUser.serverHasTalk
                    icon.source: "qrc:///client/theme/white/talk-app.svg"
                    onClicked: UserModel.openCurrentAccountTalk()
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
                                }
                            }
                        }
                    }
                }
            }
        }   // Rectangle trayWindowHeaderBackground

        ListView {
            id: activityListView

            anchors.top: trayWindowHeaderBackground.bottom
            anchors.horizontalCenter: trayWindowBackground.horizontalCenter
            width:  Style.trayWindowWidth - Style.trayWindowBorderWidth
            height: notificationLayer.visible ? Style.trayWindowHeight - Style.trayWindowHeaderHeight - Style.notificationHeight
                                              : Style.trayWindowHeight - Style.trayWindowHeaderHeight
            clip: true
            ScrollBar.vertical: ScrollBar {
                id: listViewScrollbar
            }

            model: activityModel

            delegate: RowLayout {
                id: activityItem

                width: parent.width
                height: Style.trayWindowHeaderHeight
                spacing: 0

                MouseArea {
                    id: activityMouseArea
                    enabled: (path !== "" || link !== "")
                    anchors.left: activityItem.left
                    anchors.right: ((shareButton.visible) ? shareButton.left : activityItem.right)
                    height: parent.height
                    anchors.margins: 2
                    hoverEnabled: true
                    onClicked: {
                        if (path !== "") {
                            Qt.openUrlExternally(path)
                        } else {
                            Qt.openUrlExternally(link)
                        }
                    }

                    Rectangle {
                        anchors.fill: parent
                        color: (parent.containsMouse ? Style.lightHover : "transparent")
                    }
                }

                Image {
                    id: activityIcon
                    anchors.left: activityItem.left
                    anchors.leftMargin: 8
                    anchors.rightMargin: 8
                    Layout.preferredWidth: shareButton.icon.width
                    Layout.preferredHeight: shareButton.icon.height
                    verticalAlignment: Qt.AlignCenter
                    cache: true
                    source: icon
                    sourceSize.height: 64
                    sourceSize.width: 64
                }

                Column {
                    id: activityTextColumn
                    anchors.left: activityIcon.right
                    anchors.leftMargin: 8
                    spacing: 4
                    Layout.alignment: Qt.AlignLeft
                    Text {
                        id: activityTextTitle
                        text: (type === "Activity" || type === "Notification") ? subject : message
                        width: Style.activityLabelBaseWidth + ((path === "") ? activityItem.height : 0) + ((link === "") ? activityItem.height : 0) - 8
                        elide: Text.ElideRight
                        font.pixelSize: Style.topLinePixelSize
                        color: activityTextTitleColor
                    }

                    Text {
                        id: activityTextInfo
                        text: (type === "Sync") ? displayPath
                            : (type === "File") ? subject
                            : (type === "Notification") ? message
                            : ""
                        height: (text === "") ? 0 : activityTextTitle.height
                        width: Style.activityLabelBaseWidth + ((path === "") ? activityItem.height : 0) + ((link === "") ? activityItem.height : 0) - 8
                        elide: Text.ElideRight
                        font.pixelSize: Style.subLinePixelSize
                    }

                    Text {
                        id: activityTextDateTime
                        text: dateTime
                        height: (text === "") ? 0 : activityTextTitle.height
                        width: Style.activityLabelBaseWidth + ((path === "") ? activityItem.height : 0) + ((link === "") ? activityItem.height : 0) - 8
                        elide: Text.ElideRight
                        font.pixelSize: Style.subLinePixelSize
                        color: "#808080"
                    }

                    ToolTip {
                        id: toolTip
                        visible: activityMouseArea.containsMouse
                        text: activityTextTitle.text + ((activityTextInfo.text !== "") ? "\n\n" + activityTextInfo.text : "")
                        delay: 250
                        timeout: 10000
                        // Can be dropped on more recent Qt, but on 5.12 it doesn't wrap...
                        contentItem: Text {
                            text: toolTip.text
                            font: toolTip.font
                            wrapMode: Text.Wrap
                            color: toolTip.palette.toolTipText
                        }
                    }
                }
                Button {
                    id: shareButton
                    anchors.right: activityItem.right

                    Layout.preferredWidth: (path === "") ? 0 : parent.height
                    Layout.preferredHeight: parent.height
                    Layout.alignment: Qt.AlignRight
                    flat: true
                    hoverEnabled: true
                    visible: (path === "") ? false : true
                    display: AbstractButton.IconOnly
                    icon.source: "qrc:///client/theme/share.svg"
                    icon.color: "transparent"
                    background: Rectangle {
                        color: parent.hovered ? Style.lightHover : "transparent"
                    }
                    ToolTip.visible: hovered
                    ToolTip.delay: 1000
                    ToolTip.text: qsTr("Open share dialog")
                    onClicked: Systray.openShareDialog(displayPath,absolutePath)
                }
            }

            /*add: Transition {
                NumberAnimation { properties: "y"; from: -60; duration: 100; easing.type: Easing.Linear }
            }

            remove: Transition {
                NumberAnimation { property: "opacity"; from: 1.0; to: 0; duration: 100 }
            }

            removeDisplaced: Transition {
                SequentialAnimation {
                    PauseAnimation { duration: 100}
                    NumberAnimation { properties: "y"; duration: 100; easing.type: Easing.Linear }
                }
            }

            displaced: Transition {
                NumberAnimation { properties: "y"; duration: 100; easing.type: Easing.Linear }
            }*/

        }   // ListView

        Notification {
            id: notificationLayer
            visible: Systray.notification !== ""
            text: Systray.notification
        }

    }   // Rectangle trayWindowBackground
}
