import QtQuick
import QtQuick.Layouts
import QtQuick.Controls.Material

ColumnLayout {
    RowLayout {
        Text {
            text: "Project Name"
            font.pointSize: 12
        }
        TextField {
            id: projectname
            text: backend.name
        }
    }

    RowLayout {
        Text {
            text: "Title"
            font.pointSize: 12
        }
        TextField {
            id: title
            text: ""
        }
    }
}
