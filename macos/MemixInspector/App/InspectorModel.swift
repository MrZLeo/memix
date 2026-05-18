import AppKit
import Foundation
import MemixInspectorBridge

extension MMXObjectType {
    var title: String {
        switch self {
        case .agents:
            return "Agents"
        case .chat:
            return "Chat"
        case .documents:
            return "Documents"
        @unknown default:
            return "Unknown"
        }
    }

    var systemImage: String {
        switch self {
        case .agents:
            return "person.2"
        case .chat:
            return "bubble.left.and.bubble.right"
        case .documents:
            return "doc.text"
        @unknown default:
            return "questionmark.folder"
        }
    }
}

extension MMXStoredObject {
    var stableID: String { objectID }
}

extension MMXStoreFile {
    var stableID: String { "\(contextPath)|\(blob.objectID)" }
}

extension MMXViewArtifact {
    var stableID: String { derivationID }
}

enum InspectorBrowserMode: String, CaseIterable, Identifiable {
    case tree = "Tree"
    case blobs = "Blobs"
    case views = "Views"

    var id: String { rawValue }
}

enum TreeDisplayMode: String, CaseIterable, Identifiable {
    case outline = "Outline"
    case columns = "Columns"

    var id: String { rawValue }

    var systemImage: String {
        switch self {
        case .outline:
            return "list.bullet.indent"
        case .columns:
            return "rectangle.split.3x1"
        }
    }
}

struct FileTreeNode: Identifiable {
    let id: String
    let name: String
    let path: String
    let file: MMXStoreFile?
    let children: [FileTreeNode]
    var outlineChildren: [FileTreeNode]? {
        children.isEmpty ? nil : children
    }

    var isDirectory: Bool {
        file == nil
    }
}

private final class MutableFileTreeNode {
    let name: String
    let path: String
    var file: MMXStoreFile?
    var children: [String: MutableFileTreeNode] = [:]

    init(name: String, path: String, file: MMXStoreFile? = nil) {
        self.name = name
        self.path = path
        self.file = file
    }

    func frozen() -> FileTreeNode {
        let frozenChildren = children.values
            .sorted { lhs, rhs in
                if (lhs.file == nil) != (rhs.file == nil) {
                    return lhs.file == nil
                }
                return lhs.name.localizedStandardCompare(rhs.name) == .orderedAscending
            }
            .map { $0.frozen() }

        return FileTreeNode(
            id: file?.stableID ?? "dir:\(path)",
            name: name,
            path: path,
            file: file,
            children: frozenChildren
        )
    }
}

@MainActor
final class InspectorModel: ObservableObject {
    @Published var selectedType: MMXObjectType = .agents {
        didSet {
            guard oldValue != selectedType else {
                return
            }
            selectedFileID = nil
            selectedObjectID = nil
            previewText = ""
            refresh()
        }
    }

    @Published var browserMode: InspectorBrowserMode = .tree
    @Published var treeDisplayMode: TreeDisplayMode = .outline
    @Published private(set) var rootURL: URL?
    @Published private(set) var files: [MMXStoreFile] = []
    @Published private(set) var activeViewFiles: [MMXStoreFile] = []
    @Published private(set) var objects: [MMXStoredObject] = []
    @Published private(set) var viewArtifacts: [MMXViewArtifact] = []
    @Published private(set) var activeViewArtifact: MMXViewArtifact?
    @Published var selectedFileID: String? {
        didSet { loadSelectedFilePreview() }
    }
    @Published var selectedObjectID: String?
    @Published var selectedViewID: String?
    @Published private(set) var previewText: String = ""
    @Published private(set) var status: String = ""
    @Published private(set) var errorMessage: String?

    private var store: MMXStore?

    init() {
        openDefaultStore()
    }

    var selectedFile: MMXStoreFile? {
        currentTreeFiles.first { $0.stableID == selectedFileID }
    }

    var selectedObject: MMXStoredObject? {
        objects.first { $0.stableID == selectedObjectID }
    }

    var selectedViewArtifact: MMXViewArtifact? {
        viewArtifacts.first { $0.stableID == selectedViewID }
    }

    var currentTreeFiles: [MMXStoreFile] {
        activeViewArtifact == nil ? files : activeViewFiles
    }

    var treeNodes: [FileTreeNode] {
        buildTreeNodes(from: currentTreeFiles)
    }

    var isInsideView: Bool {
        activeViewArtifact != nil
    }

    func openDefaultStore() {
        do {
            let root = try MMXStore.defaultStoreRoot()
            try openStore(rootURL: root)
        } catch {
            errorMessage = error.localizedDescription
            status = "No store loaded"
        }
    }

    func chooseStoreRoot() {
        let panel = NSOpenPanel()
        panel.canChooseDirectories = true
        panel.canChooseFiles = false
        panel.allowsMultipleSelection = false
        panel.prompt = "Open"
        if let rootURL {
            panel.directoryURL = rootURL
        }

        guard panel.runModal() == .OK, let url = panel.url else {
            return
        }

        do {
            try openStore(rootURL: url)
        } catch {
            errorMessage = error.localizedDescription
        }
    }

    func selectObjectType(_ type: MMXObjectType) {
        guard activeViewArtifact == nil else {
            return
        }
        selectedType = type
    }

    func refresh() {
        guard let store else {
            openDefaultStore()
            return
        }

        do {
            do {
                viewArtifacts = try store.listFileTreeViews()
            } catch {
                viewArtifacts = []
            }
            if let activeViewArtifact {
                if let refreshed = viewArtifacts.first(where: { $0.stableID == activeViewArtifact.stableID }) {
                    self.activeViewArtifact = refreshed
                }
                activeViewFiles = try store.listFilesInFileTreeView(derivationID: activeViewArtifact.derivationID)
                objects = visibleObjects(in: activeViewFiles)
                files = []
            } else {
                objects = try store.listObjects(type: selectedType)
                do {
                    files = try store.listFiles(type: selectedType, generation: nil)
                } catch {
                    files = []
                }
                activeViewFiles = []
            }
            errorMessage = nil
            updateStatus()
            loadSelectedFilePreview()
        } catch {
            errorMessage = error.localizedDescription
            status = "Refresh failed"
        }
    }

    func ingestFile() {
        guard let store else {
            return
        }
        guard activeViewArtifact == nil else {
            errorMessage = "Exit the locked view before ingesting."
            return
        }

        let panel = NSOpenPanel()
        panel.canChooseFiles = true
        panel.canChooseDirectories = false
        panel.allowsMultipleSelection = false
        panel.prompt = "Ingest"

        guard panel.runModal() == .OK, let sourceURL = panel.url else {
            return
        }

        do {
            let result = try store.ingestFile(
                at: sourceURL,
                type: selectedType,
                contextPath: sourceURL.lastPathComponent,
                message: "ingested from Memix Inspector"
            )
            selectedFileID = "\(result.contextPath)|\(result.blobID)"
            refresh()
            status = "Ingested \(result.contextPath)"
        } catch {
            errorMessage = error.localizedDescription
        }
    }

    func buildViewFromCurrentTree() {
        guard let store else {
            return
        }

        do {
            let paths = currentTreeFiles.map(\.contextPath)
            let artifact = try store.buildFileTree(
                name: "inspector-file-tree",
                sourceType: selectedType,
                requiredSourcePaths: paths,
                optionalSourcePaths: [],
                generation: activeViewArtifact?.sourceGeneration,
                ref: nil
            )
            refresh()
            selectedViewID = artifact.derivationID
            browserMode = .views
            status = "Built view \(shortID(artifact.derivationID))"
        } catch {
            errorMessage = error.localizedDescription
        }
    }

    func enterSelectedView() {
        guard let artifact = selectedViewArtifact else {
            return
        }

        if selectedType != artifact.sourceType {
            selectedType = artifact.sourceType
        }
        activeViewArtifact = artifact
        selectedFileID = nil
        selectedObjectID = nil
        previewText = ""
        browserMode = .tree
        refresh()
    }

    func exitView() {
        activeViewArtifact = nil
        activeViewFiles = []
        selectedFileID = nil
        previewText = ""
        refresh()
    }

    func shortID(_ id: String) -> String {
        String(id.prefix(12))
    }

    func byteCount(_ bytes: UInt64) -> String {
        ByteCountFormatter.string(fromByteCount: Int64(bytes), countStyle: .file)
    }

    private func openStore(rootURL: URL) throws {
        store = try MMXStore(rootURL: rootURL)
        self.rootURL = rootURL
        activeViewArtifact = nil
        activeViewFiles = []
        selectedFileID = nil
        selectedObjectID = nil
        selectedViewID = nil
        previewText = ""
        refresh()
    }

    private func loadSelectedFilePreview() {
        guard let store, let file = selectedFile else {
            previewText = ""
            return
        }

        do {
            let data: Data
            if let activeViewArtifact {
                data = try store.readFileTreeViewData(
                    derivationID: activeViewArtifact.derivationID,
                    contextPath: file.contextPath
                )
            } else {
                data = try store.readFileData(
                    type: selectedType,
                    contextPath: file.contextPath,
                    generation: nil
                )
            }
            if let text = String(data: data, encoding: .utf8) {
                previewText = text
            } else {
                previewText = "Binary data, \(byteCount(UInt64(data.count)))"
            }
        } catch {
            previewText = ""
            errorMessage = error.localizedDescription
        }
    }

    private func updateStatus() {
        if let activeViewArtifact {
            status = "View \(shortID(activeViewArtifact.derivationID)) locked to \(shortID(activeViewArtifact.sourceGeneration)): \(activeViewFiles.count) files, \(objects.count) blobs"
        } else {
            status = "\(objects.count) blobs, \(files.count) tree files, \(viewArtifacts.count) views"
        }
    }

    private func visibleObjects(in files: [MMXStoreFile]) -> [MMXStoredObject] {
        var seen = Set<String>()
        return files.compactMap { file in
            let inserted = seen.insert(file.blob.objectID).inserted
            return inserted ? file.blob : nil
        }
    }

    private func buildTreeNodes(from files: [MMXStoreFile]) -> [FileTreeNode] {
        let root = MutableFileTreeNode(name: "", path: "")
        for file in files {
            let components = file.contextPath
                .split(separator: "/", omittingEmptySubsequences: true)
                .map(String.init)
            guard let filename = components.last else {
                continue
            }

            var parent = root
            var currentPath = ""
            for component in components.dropLast() {
                currentPath = currentPath.isEmpty ? component : "\(currentPath)/\(component)"
                if parent.children[component] == nil {
                    parent.children[component] = MutableFileTreeNode(
                        name: component,
                        path: currentPath
                    )
                }
                parent = parent.children[component]!
            }

            let filePath = file.contextPath
            parent.children[filename] = MutableFileTreeNode(
                name: filename,
                path: filePath,
                file: file
            )
        }

        return root.frozen().children
    }
}
