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

    var locatorToken: String {
        switch self {
        case .agents:
            return "agents"
        case .chat:
            return "chat"
        case .documents:
            return "documents"
        @unknown default:
            return "unknown"
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

extension MMXViewTargetKind {
    var title: String {
        switch self {
        case .fileTree:
            return "File Tree"
        case .contextPack:
            return "Context Pack"
        @unknown default:
            return "Unknown"
        }
    }
}

extension MMXStoredObject {
    var stableID: String { objectID }
}

extension MMXStoreFile {
    var stableID: String { "\(contextPath)|\(blob.objectID)" }
}

extension MMXResolvedViewFile {
    var stableID: String { "\(outputPath)|\(blob.objectID)" }
}

extension MMXViewEnvironment {
    var stableID: String { environmentID }
}

struct InspectorFile: Identifiable {
    let id: String
    let type: MMXObjectType
    let contextPath: String
    let sourcePath: String?
    let sourceGeneration: String?
    let locator: String?
    let blob: MMXStoredObject
    let isResolvedViewFile: Bool

    init(storeFile: MMXStoreFile) {
        id = storeFile.stableID
        type = storeFile.type
        contextPath = storeFile.contextPath
        sourcePath = nil
        sourceGeneration = nil
        locator = nil
        blob = storeFile.blob
        isResolvedViewFile = false
    }

    init(resolvedFile: MMXResolvedViewFile) {
        id = resolvedFile.stableID
        type = resolvedFile.type
        contextPath = resolvedFile.outputPath
        sourcePath = resolvedFile.sourcePath
        sourceGeneration = resolvedFile.sourceGeneration
        locator = resolvedFile.locator
        blob = resolvedFile.blob
        isResolvedViewFile = true
    }

    var sourceLocator: String {
        if let sourcePath, let sourceGeneration, !sourceGeneration.isEmpty {
            return "\(type.locatorToken)/\(sourcePath)@\(sourceGeneration)"
        }
        if let locator {
            return locator
        }
        return "\(type.locatorToken)/\(contextPath)@latest"
    }
}

enum InspectorBrowserMode: String, CaseIterable, Identifiable {
    case tree = "Tree"
    case blobs = "Blobs"
    case environments = "Environments"

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
    let file: InspectorFile?
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
    var file: InspectorFile?
    var children: [String: MutableFileTreeNode] = [:]

    init(name: String, path: String, file: InspectorFile? = nil) {
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
            id: file?.id ?? "dir:\(path)",
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
    @Published private(set) var activeResolvedFiles: [MMXResolvedViewFile] = []
    @Published private(set) var objects: [MMXStoredObject] = []
    @Published private(set) var viewEnvironments: [MMXViewEnvironment] = []
    @Published private(set) var activeViewEnvironment: MMXViewEnvironment?
    @Published var selectedFileID: String? {
        didSet { loadSelectedFilePreview() }
    }
    @Published var selectedObjectID: String?
    @Published var selectedEnvironmentID: String?
    @Published private(set) var previewText: String = ""
    @Published private(set) var status: String = ""
    @Published private(set) var errorMessage: String?

    private var store: MMXStore?

    init() {
        openDefaultStore()
    }

    var selectedFile: InspectorFile? {
        currentTreeFiles.first { $0.id == selectedFileID }
    }

    var selectedObject: MMXStoredObject? {
        objects.first { $0.stableID == selectedObjectID }
    }

    var selectedViewEnvironment: MMXViewEnvironment? {
        viewEnvironments.first { $0.stableID == selectedEnvironmentID }
    }

    var currentTreeFiles: [InspectorFile] {
        if activeViewEnvironment == nil {
            return files.map(InspectorFile.init(storeFile:))
        }
        return activeResolvedFiles.map(InspectorFile.init(resolvedFile:))
    }

    var treeNodes: [FileTreeNode] {
        buildTreeNodes(from: currentTreeFiles)
    }

    var isInsideEnvironment: Bool {
        activeViewEnvironment != nil
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
        guard !isInsideEnvironment else {
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
                viewEnvironments = try store.listViewEnvironments()
            } catch {
                viewEnvironments = []
            }

            if let activeViewEnvironment {
                let refreshed = try store.resolveViewEnvironment(id: activeViewEnvironment.environmentID)
                self.activeViewEnvironment = refreshed
                activeResolvedFiles = refreshed.derivation.resolvedFiles
                objects = visibleObjects(in: activeResolvedFiles.map(InspectorFile.init(resolvedFile:)))
                files = []
            } else {
                objects = try store.listObjects(type: selectedType)
                do {
                    files = try store.listFiles(type: selectedType, generation: nil)
                } catch {
                    files = []
                }
                activeResolvedFiles = []
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
        guard !isInsideEnvironment else {
            errorMessage = "Exit the view environment before ingesting."
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

    func buildEnvironmentFromCurrentTree() {
        guard let store else {
            return
        }

        do {
            let sources = currentTreeFiles.map(\.sourceLocator)
            let environment = try store.buildViewEnvironment(sources: sources, target: .fileTree)
            refresh()
            selectedEnvironmentID = environment.environmentID
            browserMode = .environments
            status = "Built environment \(shortID(environment.environmentID))"
        } catch {
            errorMessage = error.localizedDescription
        }
    }

    func enterSelectedEnvironment() {
        guard let environment = selectedViewEnvironment else {
            return
        }

        activeViewEnvironment = environment
        activeResolvedFiles = environment.derivation.resolvedFiles
        selectedFileID = nil
        selectedObjectID = nil
        previewText = ""
        browserMode = .tree
        refresh()
    }

    func exitEnvironment() {
        activeViewEnvironment = nil
        activeResolvedFiles = []
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
        activeViewEnvironment = nil
        activeResolvedFiles = []
        selectedFileID = nil
        selectedObjectID = nil
        selectedEnvironmentID = nil
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
            if let activeViewEnvironment {
                data = try store.readFileTreeViewData(
                    environmentID: activeViewEnvironment.environmentID,
                    outputPath: file.contextPath
                )
            } else {
                data = try store.readFileData(
                    type: file.type,
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
        if let activeViewEnvironment {
            status = "Environment \(shortID(activeViewEnvironment.environmentID)): \(activeResolvedFiles.count) files, \(objects.count) blobs"
        } else {
            status = "\(objects.count) blobs, \(files.count) tree files, \(viewEnvironments.count) environments"
        }
    }

    private func visibleObjects(in files: [InspectorFile]) -> [MMXStoredObject] {
        var seen = Set<String>()
        return files.compactMap { file in
            let inserted = seen.insert("\(file.type.rawValue)|\(file.blob.objectID)").inserted
            return inserted ? file.blob : nil
        }
    }

    private func buildTreeNodes(from files: [InspectorFile]) -> [FileTreeNode] {
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
