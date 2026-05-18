import SwiftUI
import MemixInspectorBridge


struct ContentView: View {
    @EnvironmentObject private var model: InspectorModel

    var body: some View {
        NavigationSplitView {
            sidebar
        } content: {
            browser
        } detail: {
            detail
        }
        .toolbar {
            ToolbarItemGroup {
                Button {
                    model.chooseStoreRoot()
                } label: {
                    Image(systemName: "folder")
                }
                .help("Open store")

                Button {
                    model.refresh()
                } label: {
                    Image(systemName: "arrow.clockwise")
                }
                .help("Refresh")

                Button {
                    model.ingestFile()
                } label: {
                    Image(systemName: "square.and.arrow.down")
                }
                .help("Ingest file")
                .disabled(model.isInsideView)

                Button {
                    model.buildViewFromCurrentTree()
                } label: {
                    Image(systemName: "shippingbox")
                }
                .help("Build file-tree view")
                .disabled(model.currentTreeFiles.isEmpty)

                if model.isInsideView {
                    Button {
                        model.exitView()
                    } label: {
                        Image(systemName: "rectangle.portrait.and.arrow.right")
                    }
                    .help("Exit view")
                }
            }
        }
    }

    private var sidebar: some View {
        List {
            Section("Store") {
                StoreRootRow(rootURL: model.rootURL)
            }

            Section("Objects") {
                ForEach([MMXObjectType.agents, .chat, .documents], id: \.rawValue) { type in
                    Button {
                        model.selectObjectType(type)
                    } label: {
                        Label(type.title, systemImage: type.systemImage)
                            .frame(maxWidth: .infinity, alignment: .leading)
                    }
                    .buttonStyle(.plain)
                    .foregroundStyle(model.selectedType == type ? .primary : .secondary)
                    .disabled(model.isInsideView)
                    .help(model.isInsideView ? "Exit the locked view to switch object type" : type.title)
                }
            }

            if let artifact = model.activeViewArtifact {
                Section("Active View") {
                    ActiveViewRow(artifact: artifact)
                }
            }
        }
        .navigationTitle("Memix")
    }

    private var browser: some View {
        VStack(spacing: 0) {
            HStack {
                Picker("Mode", selection: $model.browserMode) {
                    ForEach(InspectorBrowserMode.allCases) { mode in
                        Text(mode.rawValue).tag(mode)
                    }
                }
                .pickerStyle(.segmented)
                .frame(width: 260)

                if model.browserMode == .tree {
                    Picker("Display", selection: $model.treeDisplayMode) {
                        ForEach(TreeDisplayMode.allCases) { mode in
                            Image(systemName: mode.systemImage)
                                .tag(mode)
                        }
                    }
                    .pickerStyle(.segmented)
                    .labelsHidden()
                    .frame(width: 74)
                    .help("Switch file browser layout")
                } else {
                    Color.clear
                        .frame(width: 74, height: 24)
                }

                if let artifact = model.activeViewArtifact {
                    Label(
                        "View \(model.shortID(artifact.derivationID)) locked \(model.shortID(artifact.sourceGeneration))",
                        systemImage: "lock"
                    )
                    .font(.caption)
                    .foregroundStyle(.secondary)
                    .lineLimit(1)
                }

                Spacer()

                Text(model.status)
                    .font(.caption)
                    .foregroundStyle(.secondary)
                    .lineLimit(1)
            }
            .frame(height: 30)
            .padding(.horizontal, 12)
            .padding(.vertical, 8)

            Divider()

            switch model.browserMode {
            case .tree:
                TreeFileList()
            case .blobs:
                BlobList()
            case .views:
                ViewArtifactList()
            }
        }
        .navigationTitle(model.activeViewArtifact == nil ? model.selectedType.title : "Locked View")
    }

    @ViewBuilder
    private var detail: some View {
        if let message = model.errorMessage {
            ErrorDetail(message: message)
        } else {
            switch model.browserMode {
            case .tree:
                FileDetail()
            case .blobs:
                ObjectDetail()
            case .views:
                ViewDetail()
            }
        }
    }
}

private struct StoreRootRow: View {
    let rootURL: URL?

    var body: some View {
        HStack(spacing: 8) {
            Image(systemName: "externaldrive")
                .foregroundStyle(.secondary)
            Text(rootURL?.path ?? "No store")
                .lineLimit(2)
                .font(.caption)
                .textSelection(.enabled)
        }
        .padding(.vertical, 4)
    }
}

private struct ActiveViewRow: View {
    @EnvironmentObject private var model: InspectorModel
    let artifact: MMXViewArtifact

    var body: some View {
        VStack(alignment: .leading, spacing: 4) {
            Label(model.shortID(artifact.derivationID), systemImage: "lock")
            Text("\(artifact.sourceType.title) \(model.shortID(artifact.sourceGeneration))")
                .font(.caption)
                .foregroundStyle(.secondary)
        }
        .padding(.vertical, 4)
    }
}

private struct TreeFileList: View {
    @EnvironmentObject private var model: InspectorModel

    var body: some View {
        Group {
            switch model.treeDisplayMode {
            case .outline:
                TreeOutlineBrowser()
            case .columns:
                TreeColumnBrowser()
            }
        }
        .overlay {
            if model.treeNodes.isEmpty {
                EmptyState(
                    title: model.isInsideView ? "No files in view" : "No tree files",
                    systemImage: "folder"
                )
            }
        }
    }
}

private struct TreeOutlineBrowser: View {
    @EnvironmentObject private var model: InspectorModel

    var body: some View {
        List(selection: $model.selectedFileID) {
            OutlineGroup(model.treeNodes, children: \.outlineChildren) { node in
                TreeNodeRow(node: node)
                    .tag(node.file?.stableID)
            }
        }
    }
}

private struct TreeColumnBrowser: View {
    @EnvironmentObject private var model: InspectorModel
    @State private var selectedIDsByDepth: [Int: String] = [:]
    @State private var columnWidths: [Int: CGFloat] = [:]

    private let defaultColumnWidth: CGFloat = 250
    private let minimumColumnWidth: CGFloat = 170
    private let maximumColumnWidth: CGFloat = 420

    private var columns: [[FileTreeNode]] {
        var result = [model.treeNodes]
        var nodes = model.treeNodes
        var depth = 0

        while let selectedID = selectedIDsByDepth[depth],
              let selected = nodes.first(where: { $0.id == selectedID }),
              !selected.children.isEmpty {
            nodes = selected.children
            result.append(nodes)
            depth += 1
        }

        return result
    }

    var body: some View {
        ScrollView(.horizontal) {
            HStack(spacing: 0) {
                ForEach(Array(columns.enumerated()), id: \.offset) { depth, nodes in
                    TreeColumn(
                        nodes: nodes,
                        width: columnWidth(for: depth),
                        selectedID: selectedIDsByDepth[depth],
                        onSelect: { select($0, at: depth) }
                    )

                    if depth < columns.count - 1 {
                        ColumnResizeHandle {
                            resizeColumn(at: depth, delta: $0)
                        }
                    }
                }
            }
            .frame(maxHeight: .infinity, alignment: .topLeading)
        }
        .background(Color(nsColor: .controlBackgroundColor))
    }

    private func select(_ node: FileTreeNode, at depth: Int) {
        selectedIDsByDepth[depth] = node.id
        for key in selectedIDsByDepth.keys where key > depth {
            selectedIDsByDepth.removeValue(forKey: key)
        }
        model.selectedFileID = node.file?.stableID
    }

    private func columnWidth(for depth: Int) -> CGFloat {
        columnWidths[depth] ?? defaultColumnWidth
    }

    private func resizeColumn(at depth: Int, delta: CGFloat) {
        let current = columnWidth(for: depth)
        columnWidths[depth] = min(max(current + delta, minimumColumnWidth), maximumColumnWidth)
    }
}

private struct TreeColumn: View {
    let nodes: [FileTreeNode]
    let width: CGFloat
    let selectedID: String?
    let onSelect: (FileTreeNode) -> Void

    var body: some View {
        ScrollView {
            LazyVStack(spacing: 1) {
                ForEach(nodes) { node in
                    Button {
                        onSelect(node)
                    } label: {
                        TreeColumnNodeRow(node: node, isSelected: node.id == selectedID)
                    }
                    .buttonStyle(.plain)
                }
            }
            .padding(.vertical, 6)
            .padding(.horizontal, 5)
        }
        .scrollIndicators(.visible)
        .frame(width: width)
        .background(Color(nsColor: .textBackgroundColor).opacity(0.56))
    }
}

private struct ColumnResizeHandle: View {
    let onDrag: (CGFloat) -> Void
    @State private var lastTranslation: CGFloat = 0

    var body: some View {
        Rectangle()
            .fill(Color(nsColor: .separatorColor))
            .frame(width: 1)
            .overlay {
                Rectangle()
                    .fill(Color.clear)
                    .frame(width: 9)
                    .contentShape(Rectangle())
                    .gesture(
                        DragGesture(minimumDistance: 0)
                            .onChanged { value in
                                let delta = value.translation.width - lastTranslation
                                lastTranslation = value.translation.width
                                onDrag(delta)
                            }
                            .onEnded { _ in
                                lastTranslation = 0
                            }
                    )
            }
    }
}

private struct TreeNodeRow: View {
    @EnvironmentObject private var model: InspectorModel
    let node: FileTreeNode

    var body: some View {
        HStack(spacing: 8) {
            Image(systemName: node.isDirectory ? "folder" : "doc.text")
                .foregroundStyle(node.isDirectory ? .secondary : .primary)
                .frame(width: 16)

            VStack(alignment: .leading, spacing: 3) {
                Text(node.name)
                    .font(.system(.body, design: node.isDirectory ? .default : .monospaced))

                if let file = node.file {
                    HStack {
                        Text(model.shortID(file.blob.objectID))
                        Text(model.byteCount(file.blob.sizeBytes))
                    }
                    .font(.caption)
                    .foregroundStyle(.secondary)
                }
            }
        }
        .padding(.vertical, 3)
    }
}

private struct TreeColumnNodeRow: View {
    @EnvironmentObject private var model: InspectorModel
    let node: FileTreeNode
    let isSelected: Bool

    var body: some View {
        HStack(spacing: 8) {
            Image(systemName: node.isDirectory ? "folder" : "doc.text")
                .symbolRenderingMode(.hierarchical)
                .foregroundStyle(node.isDirectory ? .secondary : .primary)
                .frame(width: 16)

            Text(node.name)
                .lineLimit(1)
                .font(.system(size: 13, weight: node.isDirectory ? .medium : .regular, design: node.isDirectory ? .default : .monospaced))
                .foregroundStyle(.primary)

            Spacer(minLength: 8)

            if let file = node.file {
                Text(model.byteCount(file.blob.sizeBytes))
                    .font(.caption2)
                    .foregroundStyle(.secondary)
                    .lineLimit(1)
            }

            if node.isDirectory {
                Image(systemName: "chevron.right")
                    .font(.caption)
                    .foregroundStyle(.tertiary)
            }
        }
        .frame(height: 28)
        .padding(.horizontal, 8)
        .contentShape(Rectangle())
        .background(
            RoundedRectangle(cornerRadius: 6)
                .fill(isSelected ? Color.accentColor.opacity(0.22) : Color.clear)
        )
    }
}

private struct BlobList: View {
    @EnvironmentObject private var model: InspectorModel

    var body: some View {
        List(selection: $model.selectedObjectID) {
            ForEach(model.objects, id: \.stableID) { object in
                VStack(alignment: .leading, spacing: 4) {
                    Text(object.filename.isEmpty ? model.shortID(object.objectID) : object.filename)
                        .font(.system(.body, design: .monospaced))
                    HStack {
                        Text(model.shortID(object.objectID))
                        Text(model.byteCount(object.sizeBytes))
                        Text(object.createdAt)
                    }
                    .font(.caption)
                    .foregroundStyle(.secondary)
                }
                .padding(.vertical, 4)
                .tag(object.stableID)
            }
        }
        .overlay {
            if model.objects.isEmpty {
                EmptyState(title: "No blobs", systemImage: "doc")
            }
        }
    }
}

private struct ViewArtifactList: View {
    @EnvironmentObject private var model: InspectorModel

    var body: some View {
        List(selection: $model.selectedViewID) {
            ForEach(model.viewArtifacts, id: \.stableID) { artifact in
                VStack(alignment: .leading, spacing: 4) {
                    Text(model.shortID(artifact.derivationID))
                        .font(.system(.body, design: .monospaced))
                    HStack {
                        Text("\(artifact.inputBlobIDs.count) inputs")
                        Text(artifact.sourceType.title)
                        Text(model.shortID(artifact.sourceGeneration))
                    }
                    .font(.caption)
                    .foregroundStyle(.secondary)
                }
                .padding(.vertical, 4)
                .tag(artifact.stableID)
            }
        }
        .overlay {
            if model.viewArtifacts.isEmpty {
                EmptyState(title: "No views", systemImage: "shippingbox")
            }
        }
    }
}

private struct FileDetail: View {
    @EnvironmentObject private var model: InspectorModel

    var body: some View {
        if let file = model.selectedFile {
            VStack(spacing: 0) {
                InspectorHeader(
                    title: file.contextPath,
                    subtitle: model.activeViewArtifact == nil
                        ? file.blob.objectID
                        : "locked \(file.blob.objectID)"
                )
                Divider()
                VSplitView {
                    ScrollView {
                        Text(model.previewText)
                            .font(.system(.body, design: .monospaced))
                            .textSelection(.enabled)
                            .frame(maxWidth: .infinity, alignment: .topLeading)
                            .padding(16)
                    }
                    .frame(minHeight: 160)

                    ScrollView {
                        MetadataGrid(rows: fileRows(file))
                            .padding(16)
                            .frame(maxWidth: .infinity, alignment: .topLeading)
                    }
                    .frame(minHeight: 120, idealHeight: 220)
                }
                .frame(maxWidth: .infinity, maxHeight: .infinity)
            }
        } else {
            EmptyState(title: "Select a file", systemImage: "doc.text.magnifyingglass")
        }
    }

    private func fileRows(_ file: MMXStoreFile) -> [(String, String)] {
        var rows: [(String, String)] = []
        if let artifact = model.activeViewArtifact {
            rows.append(("View", artifact.derivationID))
            rows.append(("Source type", artifact.sourceType.title))
            rows.append(("Locked generation", artifact.sourceGeneration))
        }
        rows.append(contentsOf: [
            ("Blob", file.blob.objectID),
            ("Hash", file.blob.hashValue),
            ("Size", model.byteCount(file.blob.sizeBytes)),
            ("Created", file.blob.createdAt),
            ("Object", file.blob.objectDirectory),
            ("Metadata", file.blob.metadataPath),
        ])
        return rows
    }
}

private struct ObjectDetail: View {
    @EnvironmentObject private var model: InspectorModel

    var body: some View {
        if let object = model.selectedObject {
            VStack(spacing: 0) {
                InspectorHeader(
                    title: object.filename.isEmpty ? model.shortID(object.objectID) : object.filename,
                    subtitle: object.objectID
                )
                Divider()
                MetadataGrid(rows: [
                    ("Type", object.type.title),
                    ("Hash", object.hashValue),
                    ("Size", model.byteCount(object.sizeBytes)),
                    ("Created", object.createdAt),
                    ("Content", object.contentPath),
                    ("Object", object.objectDirectory),
                    ("Metadata", object.metadataPath),
                ])
                .padding(16)
                Spacer()
            }
        } else {
            EmptyState(title: "Select a blob", systemImage: "doc")
        }
    }
}

private struct ViewDetail: View {
    @EnvironmentObject private var model: InspectorModel

    var body: some View {
        if let artifact = model.selectedViewArtifact {
            VStack(spacing: 0) {
                HStack(alignment: .top) {
                    InspectorHeader(title: model.shortID(artifact.derivationID), subtitle: artifact.derivationID)
                    Button {
                        model.enterSelectedView()
                    } label: {
                        Label("Enter View", systemImage: "arrow.right.square")
                    }
                    .padding(.top, 16)
                    .padding(.trailing, 16)
                }
                Divider()
                MetadataGrid(rows: [
                    ("Expression", artifact.expressionID),
                    ("Source type", artifact.sourceType.title),
                    ("Source generation", artifact.sourceGeneration),
                    ("Inputs", "\(artifact.inputBlobIDs.count)"),
                    ("Manifest", artifact.manifestPath),
                    ("Tree", artifact.treePath),
                    ("Inserted", artifact.inserted ? "true" : "false"),
                ])
                .padding(16)
                Spacer()
            }
        } else {
            EmptyState(title: "Select a view", systemImage: "shippingbox")
        }
    }
}

private struct ErrorDetail: View {
    let message: String

    var body: some View {
        VStack(spacing: 10) {
            Image(systemName: "exclamationmark.triangle")
                .font(.system(size: 32))
                .foregroundStyle(.secondary)
            Text("Memix error")
                .font(.headline)
            Text(message)
                .foregroundStyle(.secondary)
                .textSelection(.enabled)
        }
        .padding(24)
        .frame(maxWidth: .infinity, maxHeight: .infinity)
    }
}

private struct EmptyState: View {
    let title: String
    let systemImage: String

    var body: some View {
        VStack(spacing: 10) {
            Image(systemName: systemImage)
                .font(.system(size: 32))
                .foregroundStyle(.secondary)
            Text(title)
                .font(.headline)
                .foregroundStyle(.secondary)
        }
        .padding(24)
        .frame(maxWidth: .infinity, maxHeight: .infinity)
    }
}

private struct InspectorHeader: View {
    let title: String
    let subtitle: String

    var body: some View {
        VStack(alignment: .leading, spacing: 6) {
            Text(title)
                .font(.title3.weight(.semibold))
                .lineLimit(2)
                .textSelection(.enabled)
            Text(subtitle)
                .font(.system(.caption, design: .monospaced))
                .foregroundStyle(.secondary)
                .lineLimit(2)
                .textSelection(.enabled)
        }
        .frame(maxWidth: .infinity, alignment: .leading)
        .padding(16)
    }
}

private struct MetadataGrid: View {
    let rows: [(String, String)]

    var body: some View {
        Grid(alignment: .leading, horizontalSpacing: 16, verticalSpacing: 10) {
            ForEach(rows, id: \.0) { row in
                GridRow {
                    Text(row.0)
                        .foregroundStyle(.secondary)
                    Text(row.1)
                        .font(.system(.body, design: .monospaced))
                        .textSelection(.enabled)
                        .lineLimit(3)
                }
            }
        }
        .frame(maxWidth: .infinity, alignment: .topLeading)
    }
}
