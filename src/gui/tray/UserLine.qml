import QtQuick 2.9
import QtQuick.Window 2.3
import QtQuick.Controls 2.2
import QtQuick.Layouts 1.2

// Custom qml modules are in /theme (and included by resources.qrc)
import Style 1.0
import com.nextcloud.desktopclient 1.0

MenuItem {
    id: userLine
    height: Style.trayWindowHeaderHeight

    Accessible.role: Accessible.MenuItem
    Accessible.name: qsTr("Account entry")

    RowLayout {
        id: userLineLayout
        spacing: 0
        width: Style.currentAccountButtonWidth
        height: parent.height

        Button {
            id: accountButton
            Layout.preferredWidth: (userLineLayout.width * (5/6))
            Layout.preferredHeight: (userLineLayout.height)
            display: AbstractButton.IconOnly
            hoverEnabled: true
            flat: true

            Accessible.role: Accessible.Button
            Accessible.name: qsTr("Switch to account") + " " + name

            MouseArea {
                anchors.fill: parent
                hoverEnabled: true
                onContainsMouseChanged: {
                    accountStatusIndicatorBackground.color = (containsMouse ? "#f6f6f6" : "white")
                }
                onClicked: {
                    if (!isCurrentUser) {
                        UserModel.switchCurrentUser(id)
                    } else {
                        accountMenu.close()
                    }
                }
            }


            background: Item {
                height: parent.height
                width: userLine.menu ? userLine.menu.width : 0
                Rectangle {
                    anchors.fill: parent
                    anchors.margins: 1
                    color: parent.parent.hovered ? Style.lightHover : "transparent"
                }
            }

            RowLayout {
                id: accountControlRowLayout
                height: accountButton.height
                width: accountButton.width
                spacing: Style.userStatusSpacing
                Image {
                    id: accountAvatar
                    Layout.leftMargin: 7
                    verticalAlignment: Qt.AlignCenter
                    cache: false
                    source: model.avatar != "" ? model.avatar : "image://avatars/fallbackBlack"
                    Layout.preferredHeight: Style.accountAvatarSize
                    Layout.preferredWidth: Style.accountAvatarSize
                    Rectangle {
                        id: accountStatusIndicatorBackground
                        visible: model.isConnected &&
                                 model.serverHasUserStatus
                        width: accountStatusIndicator.sourceSize.width + 2
                        height: width
                        anchors.bottom: accountAvatar.bottom
                        anchors.right: accountAvatar.right
                        color: "white"
                        radius: width*0.5
                    }
                    Image {
                        id: accountStatusIndicator
                        visible: model.isConnected &&
                                 model.serverHasUserStatus
                        source: model.statusIcon
                        cache: false
                        x: accountStatusIndicatorBackground.x + 1
                        y: accountStatusIndicatorBackground.y + 1
                        sourceSize.width: Style.accountAvatarStateIndicatorSize
                        sourceSize.height: Style.accountAvatarStateIndicatorSize

                        Accessible.role: Accessible.Indicator
                        Accessible.name: model.desktopNotificationsAllowed ? qsTr("Current user status is online") : qsTr("Current user status is do not disturb")
                    }
                }

                Column {
                    id: accountLabels
                    Layout.leftMargin: Style.accountLabelsSpacing
                    Label {
                        id: accountUser
                        width: 128
                        text: name
                        elide: Text.ElideRight
                        color: "black"
                        font.pixelSize: Style.topLinePixelSize
                        font.bold: true
                    }
                    Row {
                        visible: model.isConnected &&
                                 model.serverHasUserStatus
                        width: Style.currentAccountLabelWidth + Style.userStatusEmojiSize
                        Label {
                            id: emoji
                            height: Style.topLinePixelSize
                            visible: model.statusEmoji !== ""
                            text: statusEmoji
                            topPadding: -Style.accountLabelsSpacing
                        }
                        Label {
                            id: message
                            height: Style.topLinePixelSize
                            visible: model.statusMessage !== ""
                            text: statusMessage
                            elide: Text.ElideRight
                            color: "black"
                            font.pixelSize: Style.subLinePixelSize
                            leftPadding: Style.accountLabelsSpacing
                        }
                    }
                    Label {
                        id: accountServer
                        width: Style.currentAccountLabelWidth
                        height: Style.topLinePixelSize
                        text: server
                        elide: Text.ElideRight
                        color: "black"
                        font.pixelSize: Style.subLinePixelSize
                    }
                }
            }
        } // accountButton

        Button {
            id: userMoreButton
            Layout.preferredWidth: (userLineLayout.width * (1/6))
            Layout.preferredHeight: userLineLayout.height
            flat: true

            icon.source: "qrc:///client/theme/more.svg"
            icon.color: "transparent"

            Accessible.role: Accessible.ButtonMenu
            Accessible.name: qsTr("Account actions")
            Accessible.onPressAction: userMoreButtonMouseArea.clicked()

            MouseArea {
                id: userMoreButtonMouseArea
                anchors.fill: parent
                hoverEnabled: true
                onClicked: {
                    if (userMoreButtonMenu.visible) {
                        userMoreButtonMenu.close()
                    } else {
                        userMoreButtonMenu.popup()
                    }
                }
            }
            background:
                Rectangle {
                color: userMoreButtonMouseArea.containsMouse ? "grey" : "transparent"
                opacity: 0.2
                height: userMoreButton.height - 2
                y: userMoreButton.y + 1
            }

            Menu {
                id: userMoreButtonMenu
                width: 120
                closePolicy: Menu.CloseOnPressOutsideParent | Menu.CloseOnEscape

                background: Rectangle {
                    border.color: Style.menuBorder
                    radius: 2
                }

                MenuItem {
                    text: model.isConnected ? qsTr("Log out") : qsTr("Log in")
                    font.pixelSize: Style.topLinePixelSize
                    hoverEnabled: true
                    onClicked: {
                        model.isConnected ? UserModel.logout(index) : UserModel.login(index)
                        accountMenu.close()
                    }

                    background: Item {
                        height: parent.height
                        width: parent.menu.width
                        Rectangle {
                            anchors.fill: parent
                            anchors.margins: 1
                            color: parent.parent.hovered ? Style.lightHover : "transparent"
                        }
                    }

                    Accessible.role: Accessible.Button
                    Accessible.name: model.isConnected ? qsTr("Log out") : qsTr("Log in")

                    onPressed: {
                        if (model.isConnected) {
                            UserModel.logout(index)
                        } else {
                            UserModel.login(index)
                        }
                        accountMenu.close()
                    }
                }

                MenuItem {
                    id: removeAccountButton
                    text: qsTr("Remove account")
                    font.pixelSize: Style.topLinePixelSize
                    hoverEnabled: true
                    onClicked: {
                        UserModel.removeAccount(index)
                        accountMenu.close()
                    }

                    background: Item {
                        height: parent.height
                        width: parent.menu.width
                        Rectangle {
                            anchors.fill: parent
                            anchors.margins: 1
                            color: parent.parent.hovered ? Style.lightHover : "transparent"
                        }
                    }

                    Accessible.role: Accessible.Button
                    Accessible.name: text
                    Accessible.onPressAction: removeAccountButton.clicked()
                }
            }
        }
    }
}   // MenuItem userLine
