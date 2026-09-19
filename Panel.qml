// plugin/Panel.qml
// Omarchy Bar-Widget & Interactive Dropdown Control Panel for Sony ULT WEAR.
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Quickshell
import Quickshell.Io
import qs.Commons
import qs.Ui
import "Model.js" as Model

Panel {
  id: root
  moduleName: "io.github.felixdacraft.omasony"
  ipcTarget: "io.github.felixdacraft.omasony"
  manageIpc: false

  readonly property color foreground: bar ? bar.foreground : Color.foreground
  readonly property color urgent: bar ? bar.urgent : Color.urgent
  readonly property color barForeground: bar ? bar.barForeground : Color.foreground
  readonly property string fontFamily: bar ? bar.fontFamily : Style.font.family

  // Keyboard navigation state
  property string focusSection: "noise"
  property int noiseIndex: 0
  property int ultIndex: 0
  property bool cursorActive: false

  readonly property var noiseModes: [Model.NOISE_ANC, Model.NOISE_AMBIENT, Model.NOISE_OFF]

  implicitWidth: button.implicitWidth
  implicitHeight: button.implicitHeight

  Service {
    id: sony
    settings: root.settings
  }

  onOpenedChanged: {
    if (opened) {
      cursorActive = true
      focusSection = "noise"
      var idx = noiseModes.indexOf(sony.noiseMode)
      noiseIndex = idx !== -1 ? idx : 0
      ultIndex = sony.ultMode
      if (panelFlick) panelFlick.contentY = 0
      sony.refresh()
      Qt.callLater(function() { keyCatcher.forceActiveFocus() })
    }
  }

  function moveCursor(dx, dy) {
    cursorActive = true
    var sections = ["noise", "voice", "ult", "dsee"]

    if (dy !== 0) {
      if (focusSection === "noise") {
        if (dy > 0) {
          focusSection = "voice"
        }
      } else if (focusSection === "voice") {
        if (dy > 0) focusSection = "ult"
        else if (dy < 0) focusSection = "noise"
      } else if (focusSection === "ult") {
        if (dy > 0) focusSection = "dsee"
        else if (dy < 0) focusSection = "voice"
      } else if (focusSection === "dsee") {
        if (dy < 0) focusSection = "ult"
      }
      return
    }

    if (dx !== 0) {
      if (focusSection === "noise") {
        noiseIndex = Math.max(0, Math.min(noiseModes.length - 1, noiseIndex + dx))
      } else if (focusSection === "ult") {
        ultIndex = Math.max(Model.ULT_OFF, Math.min(Model.ULT_2, ultIndex + dx))
      }
    }
  }

  function activateCursor() {
    if (focusSection === "noise") {
      sony.setNoiseMode(noiseModes[noiseIndex])
    } else if (focusSection === "voice") {
      sony.setVoiceFocus(!sony.voicePassthrough)
    } else if (focusSection === "ult") {
      sony.setUltMode(ultIndex)
    } else if (focusSection === "dsee") {
      sony.setDsee(!sony.dseeExtreme)
    }
  }

  IpcHandler {
    target: root.ipcTarget
    function open(): void { root.open() }
    function close(): void { root.close() }
    function show(): void { root.open() }
    function hide(): void { root.close() }
    function toggle(): void { root.toggle() }
    function refresh(): string { sony.refresh(); return "ok" }
    function cycleNoise(): string { sony.cycleNoiseMode(); return sony.noiseMode }
  }

  // Bar Widget Button
  WidgetButton {
    id: button
    anchors.fill: parent
    bar: root.bar
    labelVisible: false
    hasVisualContent: true
    fixedWidth: vertical ? -1 : (contentRow.implicitWidth + scaledHorizontalMargin * 2)
    tooltipText: sony.connected
      ? ((sony.deviceName || "ULT WEAR") + " (" + Model.noiseModeName(sony.noiseMode) + ", " + Model.formatBattery(sony.batteryLevel) + ")")
      : "Sony ULT WEAR (Disconnected)"

    Row {
      id: contentRow
      anchors.centerIn: parent
      spacing: Style.space(6)

      SonyIcon {
        id: barIcon
        anchors.verticalCenter: parent.verticalCenter
        iconSize: Style.space(14)
        connected: sony.connected
        batteryLevel: sony.batteryLevel
        charging: sony.batteryCharging
        color: sony.connected
          ? (button.active ? button.activeColor : button.foreground)
          : Qt.rgba(button.foreground.r, button.foreground.g, button.foreground.b, 0.4)
      }

      Text {
        anchors.verticalCenter: parent.verticalCenter
        textFormat: Text.PlainText
        text: sony.connected ? Model.formatBattery(sony.batteryLevel) : "—"
        color: button.active ? button.activeColor : button.foreground
        font.family: root.fontFamily
        font.pixelSize: Style.font.caption
        renderType: Text.NativeRendering
        visible: !button.vertical
      }
    }

    onPressed: function(buttonCode) {
      if (buttonCode === Qt.RightButton) {
        sony.cycleNoiseMode()
      } else {
        root.toggle()
      }
    }
  }

  // Dropdown Control Panel
  KeyboardPanel {
    id: panel
    anchorItem: button
    owner: root
    bar: root.bar
    open: root.opened
    focusTarget: keyCatcher
    contentWidth: panel.fittedContentWidth(Style.space(380))
    contentHeight: panel.fittedContentHeight(panelColumn.implicitHeight + Style.space(24), Style.space(580))

    PanelKeyCatcher {
      id: keyCatcher
      anchors.fill: parent
      onCloseRequested: root.close()
      onMoveRequested: function(dx, dy) { root.moveCursor(dx, dy) }
      onActivateRequested: function() { root.activateCursor() }

      Flickable {
        id: panelFlick
        anchors.fill: parent
        contentWidth: width
        contentHeight: panelColumn.implicitHeight
        clip: true
        boundsBehavior: Flickable.StopAtBounds

        ScrollBar.vertical: ScrollBar {
          policy: panelColumn.implicitHeight > panelFlick.height ? ScrollBar.AsNeeded : ScrollBar.AlwaysOff
        }

        Column {
          id: panelColumn
          width: parent.width
          spacing: Style.space(12)

          // -------------------------------------------------------------------
          // 1. Device Header
          // -------------------------------------------------------------------
          Row {
            id: headerRow
            width: parent.width
            spacing: Style.space(12)

            SonyIcon {
              id: headerIcon
              anchors.verticalCenter: parent.verticalCenter
              iconSize: Style.space(28)
              connected: sony.connected
              batteryLevel: sony.batteryLevel
              charging: sony.batteryCharging
            }

            Column {
              anchors.verticalCenter: parent.verticalCenter
              // Claim whatever the icon and the battery block leave behind.
              // A Row disables itself entirely if a child anchors horizontally,
              // so the battery block is pushed right by sizing this instead.
              width: Math.max(0, headerRow.width - headerIcon.width - headerRow.spacing
                                 - (headerBattery.visible ? headerBattery.width + headerRow.spacing : 0))
              spacing: Style.space(2)

              Text {
                textFormat: Text.PlainText
                text: sony.connected ? (sony.deviceName || "ULT WEAR") : "ULT WEAR"
                color: root.foreground
                font.family: root.fontFamily
                font.pixelSize: Style.font.title
                font.bold: true
                elide: Text.ElideRight
                width: parent.width
              }

              Row {
                spacing: Style.space(6)
                anchors.left: parent.left

                Rectangle {
                  anchors.verticalCenter: parent.verticalCenter
                  width: Style.space(7)
                  height: Style.space(7)
                  radius: width / 2
                  color: sony.connected ? "#2ecc71" : Qt.darker(root.foreground, 1.8)
                }

                Text {
                  anchors.verticalCenter: parent.verticalCenter
                  textFormat: Text.PlainText
                  text: sony.connected
                    ? (sony.codec ? "Connected • " + sony.codec : "Connected")
                    : "Disconnected"
                  color: Qt.darker(root.foreground, 1.4)
                  font.family: root.fontFamily
                  font.pixelSize: Style.font.caption
                }
              }
            }

            // Battery status block
            Column {
              id: headerBattery
              anchors.verticalCenter: parent.verticalCenter
              spacing: Style.space(2)
              visible: sony.connected

              Row {
                anchors.right: parent.right
                spacing: Style.space(4)

                Text {
                  anchors.verticalCenter: parent.verticalCenter
                  text: Model.batteryIcon(sony.batteryLevel, sony.batteryCharging)
                  color: root.foreground
                  font.family: root.fontFamily
                  font.pixelSize: Style.font.subtitle
                }

                Text {
                  anchors.verticalCenter: parent.verticalCenter
                  textFormat: Text.PlainText
                  text: Model.formatBattery(sony.batteryLevel)
                  color: root.foreground
                  font.family: root.fontFamily
                  font.pixelSize: Style.font.subtitle
                  font.bold: true
                }
              }

              Text {
                anchors.right: parent.right
                textFormat: Text.PlainText
                visible: sony.batteryCharging
                text: "Charging"
                color: "#2ecc71"
                font.family: root.fontFamily
                font.pixelSize: Style.font.caption
              }
            }
          }

          PanelSeparator {
            foreground: root.foreground
          }

          // -------------------------------------------------------------------
          // 2. Noise Control Mode Selector
          // -------------------------------------------------------------------
          Column {
            width: parent.width
            spacing: Style.space(8)

            PanelSectionHeader {
              text: "NOISE CONTROL"
              foreground: root.foreground
              fontFamily: root.fontFamily
            }

            Row {
              id: noiseModeRow
              width: parent.width
              spacing: Style.space(6)

              readonly property real cellWidth: (width - spacing * (noiseModes.length - 1)) / noiseModes.length

              Repeater {
                model: [
                  { mode: Model.NOISE_ANC, label: "ANC", icon: Model.noiseModeIcon(Model.NOISE_ANC) },
                  { mode: Model.NOISE_AMBIENT, label: "Ambient", icon: Model.noiseModeIcon(Model.NOISE_AMBIENT) },
                  { mode: Model.NOISE_OFF, label: "Off", icon: Model.noiseModeIcon(Model.NOISE_OFF) }
                ]

                Button {
                  required property var modelData
                  required property int index
                  width: noiseModeRow.cellWidth
                  iconText: modelData.icon
                  iconSize: Style.font.title
                  text: modelData.label
                  fontSize: Style.font.bodySmall
                  foreground: root.foreground
                  fontFamily: root.fontFamily
                  bordered: true
                  selected: sony.noiseMode === modelData.mode
                  hasCursor: root.cursorActive && root.focusSection === "noise" && root.noiseIndex === index
                  onClicked: {
                    root.focusSection = "noise"
                    root.noiseIndex = index
                    sony.setNoiseMode(modelData.mode)
                  }
                }
              }
            }
          }

          // -------------------------------------------------------------------
          // 3. Ambient Voice Focus
          // -------------------------------------------------------------------
          Column {
            width: parent.width
            spacing: Style.space(8)
            opacity: sony.noiseMode === Model.NOISE_AMBIENT ? 1.0 : 0.4

            PanelSectionHeader {
              text: "AMBIENT SOUND"
              foreground: root.foreground
              fontFamily: root.fontFamily
            }

            Toggle {
              width: parent.width
              label: "Focus on Voice"
              description: "Emphasize voices while Ambient Sound is active"
              checked: sony.voicePassthrough
              enabled: sony.noiseMode === Model.NOISE_AMBIENT
              hasCursor: root.cursorActive && root.focusSection === "voice"
              foreground: root.foreground
              fontFamily: root.fontFamily
              onClicked: {
                root.focusSection = "voice"
                sony.setVoiceFocus(!sony.voicePassthrough)
              }
            }
          }

          PanelSeparator {
            foreground: root.foreground
          }

          // -------------------------------------------------------------------
          // 4. ULT Bass Selector
          // -------------------------------------------------------------------
          Column {
            width: parent.width
            spacing: Style.space(8)

            PanelSectionHeader {
              text: "ULT POWER SOUND (" + Model.ultModeName(sony.ultMode) + ")"
              foreground: root.foreground
              fontFamily: root.fontFamily
            }

            Row {
              id: ultModeRow
              width: parent.width
              spacing: Style.space(6)

              readonly property real cellWidth: (width - spacing * 2) / 3

              Repeater {
                model: [
                  { mode: Model.ULT_OFF, label: "Off" },
                  { mode: Model.ULT_1, label: "ULT 1" },
                  { mode: Model.ULT_2, label: "ULT 2" }
                ]

                Button {
                  required property var modelData
                  required property int index
                  width: ultModeRow.cellWidth
                  text: modelData.label
                  fontSize: Style.font.bodySmall
                  foreground: root.foreground
                  fontFamily: root.fontFamily
                  bordered: true
                  selected: sony.ultMode === modelData.mode
                  hasCursor: root.cursorActive && root.focusSection === "ult" && root.ultIndex === index
                  onClicked: {
                    root.focusSection = "ult"
                    root.ultIndex = index
                    sony.setUltMode(modelData.mode)
                  }
                }
              }
            }
          }

          PanelSeparator {
            foreground: root.foreground
          }

          // -------------------------------------------------------------------
          // 5. Feature Toggles
          // -------------------------------------------------------------------
          Column {
            width: parent.width
            spacing: Style.space(8)

            PanelSectionHeader {
              text: "FEATURES"
              foreground: root.foreground
              fontFamily: root.fontFamily
            }

            Toggle {
              id: toggleDsee
              width: parent.width
              label: "DSEE"
              description: "Upscales compressed tracks toward hi-res quality"
              checked: sony.dseeExtreme
              hasCursor: root.cursorActive && root.focusSection === "dsee"
              foreground: root.foreground
              fontFamily: root.fontFamily
              onClicked: {
                root.focusSection = "dsee"
                sony.setDsee(!sony.dseeExtreme)
              }
            }
          }
        }
      }
    }
  }
}
