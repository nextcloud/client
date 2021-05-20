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

    Accessible.role: Accessible.Application
    Accessible.name: qsTr("Nextcloud desktop main dialog")

    Component.onCompleted: Systray.forceWindowInit(trayWindow)

    // Close tray window when focus is lost (e.g. click somewhere else on the screen)
    onActiveChanged: {
        if(!active) {
            trayWindow.hide();
            Systray.setClosed();
        }
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
            radius: trayWindowBackground.radius
        }
    }

    Rectangle {
        id: trayWindowBackground

        anchors.fill:   parent
        radius:         Style.trayWindowRadius
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

                            Accessible.role: PopupMenu
                            Accessible.name: qsTr("Account switcher and settings menu")

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
                            spacing: Style.userStatusSpacing
                            Layout.alignment: Qt.AlignLeft
                            Layout.leftMargin: Style.userStatusSpacing
                            anchors.top: currentAccountAvatar.top
                            anchors.topMargin: Style.userStatusSpacing
                            Label {
                                id: currentAccountUser
                                width: Style.currentAccountLabelWidth
                                text: UserModel.currentUser.name
                                elide: Text.ElideRight
                                color: Style.ncTextColor
                                font.pixelSize: Style.topLinePixelSize
                                font.bold: true
                            }
                            Row {
                                id: currentUserStatus
                                visible: UserModel.currentUser.isConnected &&
                                         UserModel.currentUser.serverHasUserStatus
                                anchors.top: currentAccountUser.bottom
                                Label {
                                    id: emoji
                                    visible: UserModel.currentUser.statusEmoji !== ""
                                    width: Style.userStatusEmojiSize
                                    text: UserModel.currentUser.statusEmoji
                                }
                                Label {
                                    id: message
                                    anchors.bottom: emoji.bottom
                                    anchors.left: emoji.right
                                    anchors.leftMargin: emoji.width + Style.userStatusSpacing
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
                            }
                        }
                    }
                }

                RowLayout {
                    id: openLocalFolderRowLayout
                    spacing: 0
                    Layout.preferredWidth:  Style.trayWindowHeaderHeight
                    Layout.preferredHeight: Style.trayWindowHeaderHeight
                    
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

                        Accessible.role: Accessible.PopupMenu
                        Accessible.name: qsTr("Apps menu")

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

            delegate: RowLayout {
                id: activityItem

                readonly property variant links: model.links

                readonly property int itemIndex: model.index

                width: parent.width
                height: Style.trayWindowHeaderHeight
                spacing: 0

                Accessible.role: Accessible.ListItem
                Accessible.name: path !== "" ? qsTr("Open %1 locally").arg(displayPath)
                                             : message
                Accessible.onPressAction: activityMouseArea.clicked()

                MouseArea {
                    id: activityMouseArea
                    enabled: (path !== "" || link !== "")
                    anchors.left: activityItem.left
                    anchors.right: activityActionsLayout.right
                    height: parent.height
                    anchors.margins: 2
                    hoverEnabled: true
                    onClicked: activityModel.triggerDefaultAction(model.index)

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
                    Layout.preferredWidth: shareMenuItem.icon.width
                    Layout.preferredHeight: shareMenuItem.icon.height
                    verticalAlignment: Qt.AlignCenter
                    cache: true
                    source: icon
                    sourceSize.height: 64
                    sourceSize.width: 64
                }

                Column {
                    id: activityTextColumn
                    anchors.left: activityIcon.right
                    anchors.right: activityActionsLayout.left
                    anchors.leftMargin: 8
                    spacing: 4
                    Layout.alignment: Qt.AlignLeft
                    Text {
                        id: activityTextTitle
                        text: (type === "Activity" || type === "Notification") ? subject : message
                        width: parent.width
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
                        width: parent.width
                        elide: Text.ElideRight
                        font.pixelSize: Style.subLinePixelSize
                    }

                    Text {
                        id: activityTextDateTime
                        text: dateTime
                        height: (text === "") ? 0 : activityTextTitle.height
                        width: parent.width
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
                RowLayout {
                    id: activityActionsLayout
                    anchors.right: activityItem.right
                    spacing: 0
                    Layout.alignment: Qt.AlignRight

                    function actionButtonIcon(actionIndex) {
                        const verb = String(model.links[actionIndex].verb);
                        if (verb === "WEB" && (model.objectType === "chat" || model.objectType === "call")) {
                            return "qrc:///client/theme/reply.svg";
                        } else if (verb === "DELETE") {
                            return "qrc:///client/theme/close.svg";
                        }

                        return "qrc:///client/theme/confirm.svg";
                    }

                    Repeater {
                        model: activityItem.links.length > activityListView.maxActionButtons ? 1 : activityItem.links.length

                        ActivityActionButton {
                            id: activityActionButton

                            readonly property int actionIndex: model.index
                            readonly property bool primary: model.index === 0 && String(activityItem.links[actionIndex].verb) !== "DELETE"

                            height: activityItem.height

                            text: !primary ? "" : activityItem.links[actionIndex].label

                            imageSource: !primary ? activityActionsLayout.actionButtonIcon(actionIndex) : ""

                            textColor: primary ? Style.ncBlue : "black"
                            textColorHovered: Style.lightHover

                            textBorderColor: Style.ncBlue

                            textBgColor: "transparent"
                            textBgColorHovered: Style.ncBlue

                            tooltipText: activityItem.links[actionIndex].label

                            Layout.minimumWidth: primary ? 80 : -1
                            Layout.minimumHeight: parent.height

                            Layout.preferredWidth: primary ? -1 : parent.height

                            onClicked: activityModel.triggerAction(activityItem.itemIndex, actionIndex)
                        }

                    }

                    Button {
                        id: moreActionsButton

                        Layout.preferredWidth: parent.height
                        Layout.preferredHeight: parent.height
                        Layout.alignment: Qt.AlignRight

                        flat: true
                        hoverEnabled: true
                        visible: activityItem.links.length > activityListView.maxActionButtons
                        display: AbstractButton.IconOnly
                        icon.source: "qrc:///client/theme/more.svg"
                        icon.color: "transparent"
                        background: Rectangle {
                            color: parent.hovered ? Style.lightHover : "transparent"
                        }
                        ToolTip.visible: hovered
                        ToolTip.delay: 1000
                        ToolTip.text: qsTr("Show more actions")

                        Accessible.role: Accessible.Button
                        Accessible.name: qsTr("Show more actions")
                        Accessible.onPressAction: moreActionsButton.clicked()

                        onClicked:  moreActionsButtonContextMenu.popup();

                        Connections {
                            target: trayWindow
                            onActiveChanged: {
                                if (!trayWindow.active) {
                                    moreActionsButtonContextMenu.close();
                                }
                            }
                        }

                        Connections {
                            target: activityListView

                            onMovementStarted: {
                                moreActionsButtonContextMenu.close();
                            }
                        }

                        Container {
                            id: moreActionsButtonContextMenuContainer
                            visible: moreActionsButtonContextMenu.opened

                            width: moreActionsButtonContextMenu.width
                            height: moreActionsButtonContextMenu.height
                            anchors.right: moreActionsButton.right
                            anchors.top: moreActionsButton.top

                            Menu {
                                id: moreActionsButtonContextMenu
                                anchors.centerIn: parent

                                // transform model to contain indexed actions with primary action filtered out
                                function actionListToContextMenuList(actionList) {
                                    // early out with non-altered data
                                    if (activityItem.links.length <= activityListView.maxActionButtons) {
                                        return actionList;
                                    }

                                    // add index to every action and filter 'primary' action out
                                    var reducedActionList = actionList.reduce(function(reduced, action, index) {
                                        if (!action.primary) {
                                           var actionWithIndex = { actionIndex: index, label: action.label };
                                           reduced.push(actionWithIndex);
                                        }
                                        return reduced;
                                    }, []);


                                    return reducedActionList;
                                }

                                Repeater {
                                    id: moreActionsButtonContextMenuRepeater

                                    model: moreActionsButtonContextMenu.actionListToContextMenuList(activityItem.links)

                                    delegate: MenuItem {
                                        id: moreActionsButtonContextMenuEntry
                                        readonly property int actionIndex: model.modelData.actionIndex
                                        readonly property string label: model.modelData.label
                                        text: label
                                        onTriggered: activityModel.triggerAction(activityItem.itemIndex, actionIndex)
                                    }
                                }
                            }
                        }
                    }

                    Button {
                        id: moreFileActionsButton

                        Layout.preferredWidth: (path === "") ? 0 : parent.height
                        Layout.preferredHeight: parent.height
                        Layout.alignment: Qt.AlignRight
                        flat: true
                        hoverEnabled: true
                        visible: (path === "") ? false : true
                        display: AbstractButton.IconOnly
                        icon.source: "qrc:///client/theme/more.svg"
                        icon.color: "transparent"
                        background: Rectangle {
                            color: parent.hovered ? Style.lightHover : "transparent"
                        }
                        ToolTip.visible: hovered
                        ToolTip.delay: 1000
                        ToolTip.text: qsTr("Open share dialog")
                        onClicked: moreFileActionsContextMenu.popup()

                        Accessible.role: Accessible.Button
                        Accessible.name: qsTr("More actions on %1").arg(displayPath)
                        Accessible.onPressAction: moreFileActionsButton.clicked()
                    }

                     Menu {
                         id: moreFileActionsContextMenu
                         MenuItem {
                             id: shareMenuItem
                             
                             text: "Share"
                             onClicked: Systray.openShareDialog(displayPath, absolutePath)

                             Accessible.role: Accessible.MenuItem
                             Accessible.name: qsTr("Share %1").arg(displayPath)
                             Accessible.onPressAction: shareMenuItem.clicked()
                         }
                         MenuItem {
                             id: activityMenuItem
                             
                             text: "Activity"
                             onClicked: Systray.openFileActivityDialog(displayPath, absolutePath)

                             Accessible.role: Accessible.MenuItem
                             Accessible.name: qsTr("Share %1").arg(displayPath)
                             Accessible.onPressAction: activityMenuItem.clicked()
                         }
                    }
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
        }

    }       // Rectangle trayWindowBackground
}
