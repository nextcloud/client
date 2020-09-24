pragma Singleton

// Minimum for this is Qt 5.5
import QtQuick 2.5

QtObject {
    // Colors
    property color ncBlue:      "#0082c9"
    property color ncBlueHover: "#009dd9"
    property color lightHover:  "#f7f7f7"
    property color lightMessage:"#fafafa"
    property color menuBorder:  "#bdbdbd"

    // Fonts
    // We are using pixel size because this is cross platform comparable, point size isn't
    property int topLinePixelSize: 12
    property int subLinePixelSize: 10

    // Dimensions and sizes
    property int trayWindowWidth: 400
    property int trayWindowHeight: 510
    property int trayWindowRadius: 10
    property int trayWindowBorderWidth: 1
    property int trayWindowHeaderHeight: 60

    property int currentAccountButtonWidth: 220
    property int currentAccountButtonRadius: 2
    property int currentAccountLabelWidth: 128

    property int accountAvatarSize: (trayWindowHeaderHeight - 16)
    property int accountAvatarStateIndicatorSize: 16
    property int accountLabelWidth: 128

    property int accountDropDownCaretSize: 20
    property int accountDropDownCaretMargin: 8

    property int addAccountButtonHeight: 50

    property int headerButtonIconSize: 32

    property int activityLabelBaseWidth: 240

    property int notificationLabelWidth: 316

    property int notificationHeight: 60

    // Visual behaviour
    property bool hoverEffectsEnabled: true
}
