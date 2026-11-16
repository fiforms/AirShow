import QtQuick

// ApprovalDialog — modal approval overlay for incoming connection requests.
//
// D-01 / SEC-01: Appears when connectionBridge.approvalPending is true.
// D-13: Full-screen MouseArea blocks input to underlying content when visible.
// Works without a mouse (kiosks, remotes): it takes keyboard focus when a request
// arrives, with Allow selected. Left/Right/Tab move the selection, Enter/Return/
// Select/Space activate it, Esc denies, Y/N allow/deny directly.
// Follows the established HudOverlay pattern:
//   - visible: opacity > 0 (prevents mouse-event blocking at opacity 0, RESEARCH.md Pitfall 3)
//   - Behavior on opacity { NumberAnimation { duration: 150 } }
//   - Dark semi-transparent background matching HudOverlay aesthetic
Item {
    id: approvalDialog
    anchors.fill: parent

    // CRITICAL: use opacity > 0, not approvalPending directly
    // Prevents mouse-event blocking when faded out (RESEARCH.md Pitfall 3)
    visible: opacity > 0
    opacity: connectionBridge.approvalPending ? 1.0 : 0.0

    Behavior on opacity { NumberAnimation { duration: 150 } }

    // Keyboard selection: 0 = Allow, 1 = Deny
    property int selected: 0

    function resolve(approved) {
        if (!connectionBridge.approvalPending) return
        securityManager.resolveApproval(connectionBridge.pendingRequestId, approved)
        connectionBridge.clearApprovalRequest()
    }

    Connections {
        target: connectionBridge
        function onApprovalPendingChanged() {
            if (connectionBridge.approvalPending) {
                approvalDialog.selected = 0
                approvalDialog.forceActiveFocus()
            } else {
                approvalDialog.focus = false
            }
        }
    }

    Keys.onPressed: (event) => {
        if (!connectionBridge.approvalPending) return
        switch (event.key) {
        case Qt.Key_Left:
        case Qt.Key_Right:
        case Qt.Key_Tab:
        case Qt.Key_Backtab:
        case Qt.Key_Up:
        case Qt.Key_Down:
            selected = 1 - selected
            break
        case Qt.Key_Return:
        case Qt.Key_Enter:
        case Qt.Key_Select:
        case Qt.Key_Space:
            resolve(selected === 0)
            break
        case Qt.Key_Escape:
        case Qt.Key_Back:
        case Qt.Key_N:
            resolve(false)
            break
        case Qt.Key_Y:
            resolve(true)
            break
        default:
            return
        }
        event.accepted = true
    }

    // Full-screen semi-transparent backdrop — dims the content behind the dialog
    Rectangle {
        anchors.fill: parent
        color: "#80000000"
    }

    // Full-screen MouseArea — blocks all input to underlying content when dialog is visible.
    // acceptedButtons: Qt.AllButtons ensures no click passes through (D-13).
    MouseArea {
        anchors.fill: parent
        acceptedButtons: Qt.AllButtons
        // Consume events — do not propagate to content below
    }

    // Centered dialog card
    Rectangle {
        id: dialogCard
        anchors.centerIn: parent
        width: 420
        height: 250
        radius: 12
        color: "#CC1A1A2E"

        Column {
            anchors.centerIn: parent
            spacing: 12
            width: parent.width - 48

            // Title
            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                text: "Connection Request"
                color: "white"
                font.pixelSize: 20
                font.bold: true
                font.family: "sans-serif"
                horizontalAlignment: Text.AlignHCenter
            }

            // Device name + protocol
            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                text: connectionBridge.pendingDeviceName + " (" + connectionBridge.pendingProtocol + ")"
                color: "white"
                font.pixelSize: 16
                font.family: "sans-serif"
                horizontalAlignment: Text.AlignHCenter
                elide: Text.ElideMiddle
                width: parent.width
            }

            // Subtitle
            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                text: "wants to connect to this display"
                color: "#BBBBBB"
                font.pixelSize: 14
                font.family: "sans-serif"
                horizontalAlignment: Text.AlignHCenter
            }

            // Allow / Deny buttons
            Row {
                anchors.horizontalCenter: parent.horizontalCenter
                spacing: 16

                // Allow button
                Item {
                    id: allowButton
                    width: 130
                    height: 40

                    Rectangle {
                        anchors.fill: parent
                        radius: 6
                        color: allowMouse.containsMouse ? "#55CC55" : "#44AA44"
                        border.color: "white"
                        border.width: approvalDialog.selected === 0 ? 3 : 0
                    }

                    Text {
                        anchors.centerIn: parent
                        text: "Allow"
                        color: "white"
                        font.pixelSize: 16
                        font.bold: true
                        font.family: "sans-serif"
                    }

                    MouseArea {
                        id: allowMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: approvalDialog.resolve(true)
                    }
                }

                // Deny button
                Item {
                    id: denyButton
                    width: 130
                    height: 40

                    Rectangle {
                        anchors.fill: parent
                        radius: 6
                        color: denyMouse.containsMouse ? "#CC5555" : "#AA4444"
                        border.color: "white"
                        border.width: approvalDialog.selected === 1 ? 3 : 0
                    }

                    Text {
                        anchors.centerIn: parent
                        text: "Deny"
                        color: "white"
                        font.pixelSize: 16
                        font.bold: true
                        font.family: "sans-serif"
                    }

                    MouseArea {
                        id: denyMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: approvalDialog.resolve(false)
                    }
                }
            }

            // Keyboard hint (no mouse on kiosks / remotes)
            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                text: "Enter: select   \u2190 \u2192: switch   Esc: deny"
                color: "#999999"
                font.pixelSize: 12
                font.family: "sans-serif"
                horizontalAlignment: Text.AlignHCenter
            }
        }
    }
}
