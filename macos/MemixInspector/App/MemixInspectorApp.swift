import SwiftUI

@main
struct MemixInspectorApp: App {
    @StateObject private var model = InspectorModel()

    var body: some Scene {
        WindowGroup {
            ContentView()
                .environmentObject(model)
                .frame(minWidth: 980, minHeight: 640)
        }
        .commands {
            CommandGroup(replacing: .newItem) {}
            CommandMenu("Store") {
                Button("Open Store...") {
                    model.chooseStoreRoot()
                }
                .keyboardShortcut("o", modifiers: [.command])

                Button("Refresh") {
                    model.refresh()
                }
                .keyboardShortcut("r", modifiers: [.command])

                Button("Ingest File...") {
                    model.ingestFile()
                }
                .keyboardShortcut("i", modifiers: [.command])
            }
        }
    }
}
