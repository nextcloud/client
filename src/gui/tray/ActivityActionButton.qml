import QtQuick 2.5
import QtQuick.Controls 2.3
import Style 1.0

Item {
    id: root
    readonly property bool labelVisible: label.visible
    readonly property bool iconVisible: icon.visible

    // label value
    property string text: ""
    
    // font value
    property string font: label.font

    // icon value
    property string imageSource: ""

    // Tooltip value
    property string tooltipText: text

    // text color
    property color textColor: Style.ncTextColor
    property color textColorHovered: Style.lightHover

    // text background color
    property color textBgColor: "transparent"
    property color textBgColorHovered: Style.lightHover

    // icon background color
    property color iconBgColor: "transparent"
    property color iconBgColorHovered: Style.lightHover

    // text border color
    property color textBorderColor: "transparent"

    property alias hovered: mouseArea.containsMouse

    signal clicked()

    Accessible.role: Accessible.Button
    Accessible.name: text !== "" ? text : (tooltipText !== "" ? tooltipText : qsTr("Activity action button"))
    Accessible.onPressAction: clicked()

    // background with border around the Text
    Rectangle {
        visible: parent.labelVisible

        anchors.fill: parent

        // padding
        anchors.topMargin: 10
        anchors.bottomMargin: 10

        border.color: parent.textBorderColor
        border.width: 1

        color: parent.hovered ? parent.textBgColorHovered : parent.textBgColor

        radius: 25
    }

    // background with border around the Image
    Rectangle {
        visible: parent.iconVisible

        anchors.fill: parent

        color: parent.hovered ? parent.iconBgColorHovered : parent.iconBgColor
    }

    // label
    Text {
        id: label
        visible: parent.text !== ""
        text: parent.text
        font: parent.font
        color: parent.hovered ? parent.textColorHovered : parent.textColor
        anchors.fill: parent
        anchors.leftMargin: 10
        anchors.rightMargin: 10
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
    }

    // icon
    Image {
        id: icon
        visible: parent.imageSource !== ""
        anchors.centerIn: parent
        source: parent.imageSource
        sourceSize.width: visible ? 32 : 0
        sourceSize.height: visible ? 32 : 0
    }

    MouseArea {
        id: mouseArea
        anchors.fill: parent
        onClicked: parent.clicked()
        hoverEnabled: true
    }

    ToolTip {
        text: parent.tooltipText
        delay: 1000
        visible: text != "" && parent.hovered
    }
}
