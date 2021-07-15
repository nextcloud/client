pragma Singleton

// Minimum for this is Qt 5.5
import QtQuick 2.5

import com.nextcloud.desktopclient 1.0

QtObject {
    // Colors
    property color ncBlue:      Theme.wizardHeaderBackgroundColor
    property color ncTextColor: Theme.wizardHeaderTitleColor
    property color lightHover:  "#f7f7f7"
    property color menuBorder:  "#bdbdbd"

    // Fonts
    // We are using pixel size because this is cross platform comparable, point size isn't
    property int topLinePixelSize: 12
    property int subLinePixelSize: 10

    // Dimensions and sizes
    property int trayWindowWidth: 400
    property int trayWindowMouseAreaWidth: 396
    property int trayWindowHeight: 510
    property int trayWindowRadius: 10
    property int trayWindowBorderWidth: 1
    property int trayWindowHeaderHeight: 60

    property int currentAccountButtonWidth: 220
    property int currentAccountButtonRadius: 2
    property int currentAccountLabelWidth: 128

    property url stateOnlineImageSource: Theme.stateOnlineImageSource
    property url stateOfflineImageSource: Theme.stateOfflineImageSource

    property int accountAvatarSize: (trayWindowHeaderHeight - 16)
    property int accountAvatarStateIndicatorSize: 16
    property int folderStateIndicatorSize: 16
    property int accountLabelWidth: 128

    property int accountDropDownCaretSize: 20
    property int accountDropDownCaretMargin: 8

    property int addAccountButtonHeight: 50

    property int headerButtonIconSize: 32

    property int activityLabelBaseWidth: 240
    
    property int userStatusEmojiSize: 8
    property int userStatusSpacing: 6
    property int userStatusAnchorsMargin: 2
    property int accountServerAnchorsMargin: 10
    property int accountLabelsSpacing: 4
    property int accountLabelsAnchorsMargin: 7
    property int accountLabelsLayoutMargin: 12
    property int accountLabelsLayoutTopMargin: 10

    // Visual behaviour
    property bool hoverEffectsEnabled: true
}
