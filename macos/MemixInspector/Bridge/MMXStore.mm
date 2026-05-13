#import "MMXStore.h"

#include "memix/store.hpp"
#include "memix/view.hpp"

#include <algorithm>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

NSString *const MMXErrorDomain = @"com.memix.inspector";

NSError *MMXMakeError(NSString *message) {
  return [NSError errorWithDomain:MMXErrorDomain
                             code:1
                         userInfo:@{NSLocalizedDescriptionKey : message}];
}

NSError *MMXMakeError(const memix::StoreError &error) {
  return MMXMakeError([NSString stringWithUTF8String:error.message.c_str()]);
}

bool MMXSetError(NSError **out_error, NSError *error) {
  if (out_error != nullptr) {
    *out_error = error;
  }
  return false;
}

bool MMXSetStoreError(NSError **out_error, const memix::StoreError &error) {
  return MMXSetError(out_error, MMXMakeError(error));
}

NSString *MMXString(std::string_view value) {
  NSString *string = [[NSString alloc] initWithBytes:value.data()
                                             length:value.size()
                                           encoding:NSUTF8StringEncoding];
  return string != nil ? string : @"";
}

NSString *MMXString(const std::string &value) { return MMXString(std::string_view(value)); }

NSString *MMXPathString(const std::filesystem::path &path) {
  return MMXString(path.generic_string());
}

std::string MMXStdString(NSString *value) {
  return value == nil ? std::string() : std::string(value.UTF8String);
}

std::filesystem::path MMXPath(NSString *value) {
  return std::filesystem::path(MMXStdString(value));
}

std::filesystem::path MMXPath(NSURL *url) {
  return std::filesystem::path(MMXStdString(url.path));
}

MMXObjectType MMXObjectTypeFromMemory(memix::MemoryObjectType type) {
  switch (type) {
  case memix::MemoryObjectType::Agents:
    return MMXObjectTypeAgents;
  case memix::MemoryObjectType::Chat:
    return MMXObjectTypeChat;
  case memix::MemoryObjectType::Documents:
    return MMXObjectTypeDocuments;
  }
}

std::optional<memix::MemoryObjectType> MMXMemoryObjectType(MMXObjectType type) {
  switch (type) {
  case MMXObjectTypeAgents:
    return memix::MemoryObjectType::Agents;
  case MMXObjectTypeChat:
    return memix::MemoryObjectType::Chat;
  case MMXObjectTypeDocuments:
    return memix::MemoryObjectType::Documents;
  }
  return std::nullopt;
}

bool MMXRequireType(MMXObjectType type, memix::MemoryObjectType *out_type,
                    NSError **out_error) {
  const std::optional<memix::MemoryObjectType> parsed = MMXMemoryObjectType(type);
  if (!parsed.has_value()) {
    return MMXSetError(out_error, MMXMakeError(@"Unsupported memix object type."));
  }
  *out_type = *parsed;
  return true;
}

std::optional<std::string> MMXOptionalString(NSString *value) {
  if (value == nil || value.length == 0) {
    return std::nullopt;
  }
  return MMXStdString(value);
}

std::optional<std::string_view>
MMXOptionalStringView(const std::optional<std::string> &storage) {
  if (!storage.has_value()) {
    return std::nullopt;
  }
  return std::string_view(*storage);
}

NSArray<NSString *> *MMXStringArray(const std::vector<std::string> &values) {
  NSMutableArray<NSString *> *items = [[NSMutableArray alloc] initWithCapacity:values.size()];
  for (const std::string &value : values) {
    [items addObject:MMXString(value)];
  }
  return items;
}

NSData *MMXReadData(const std::filesystem::path &path, NSError **out_error) {
  NSURL *url = [NSURL fileURLWithPath:MMXPathString(path)];
  NSError *read_error = nil;
  NSData *data = [NSData dataWithContentsOfURL:url
                                       options:NSDataReadingMappedIfSafe
                                         error:&read_error];
  if (data == nil) {
    MMXSetError(out_error,
                read_error != nil ? read_error
                                  : MMXMakeError(@"Failed to read file data."));
  }
  return data;
}

bool MMXPathIsSafeRelative(const std::filesystem::path &path) {
  if (path.empty() || path.is_absolute()) {
    return false;
  }
  for (const std::filesystem::path &component : path) {
    if (component.empty() || component == "." || component == "..") {
      return false;
    }
  }
  return true;
}

} // namespace

@interface MMXStoredObject ()
@property(nonatomic, readwrite) NSInteger schemaVersion;
@property(nonatomic, readwrite) NSInteger objectVersion;
@property(nonatomic, readwrite) MMXObjectType type;
@property(nonatomic, copy, readwrite) NSString *objectID;
@property(nonatomic, copy, readwrite) NSString *hashAlgorithm;
@property(nonatomic, copy, readwrite) NSString *hashValue;
@property(nonatomic, readwrite) unsigned long long sizeBytes;
@property(nonatomic, copy, readwrite) NSString *filename;
@property(nonatomic, copy, readwrite) NSString *createdAt;
@property(nonatomic, copy, readwrite) NSString *contentPath;
@property(nonatomic, copy, readwrite) NSString *objectDirectory;
@property(nonatomic, copy, readwrite) NSString *metadataPath;
@end

@implementation MMXStoredObject
@end

@interface MMXStoreFile ()
@property(nonatomic, readwrite) MMXObjectType type;
@property(nonatomic, copy, readwrite) NSString *contextPath;
@property(nonatomic, strong, readwrite) MMXStoredObject *blob;
@end

@implementation MMXStoreFile
@end

@interface MMXIngestResult ()
@property(nonatomic, copy, readwrite) NSString *blobID;
@property(nonatomic, copy, readwrite) NSString *generationID;
@property(nonatomic, copy, readwrite) NSString *contextPath;
@property(nonatomic, readwrite) BOOL inserted;
@end

@implementation MMXIngestResult
@end

@interface MMXViewArtifact ()
@property(nonatomic, copy, readwrite) NSString *derivationID;
@property(nonatomic, copy, readwrite) NSString *expressionID;
@property(nonatomic, copy, readwrite) NSString *agentGeneration;
@property(nonatomic, copy, readwrite) NSArray<NSString *> *inputBlobIDs;
@property(nonatomic, copy, readwrite) NSString *manifestPath;
@property(nonatomic, copy, readwrite) NSString *treePath;
@property(nonatomic, readwrite) BOOL inserted;
@end

@implementation MMXViewArtifact
@end

namespace {

MMXStoredObject *MMXWrapStoredObject(const memix::StoredObject &object) {
  MMXStoredObject *wrapped = [[MMXStoredObject alloc] init];
  wrapped.schemaVersion = object.schema_version;
  wrapped.objectVersion = object.object_version;
  wrapped.type = MMXObjectTypeFromMemory(object.type);
  wrapped.objectID = MMXString(object.id);
  wrapped.hashAlgorithm = MMXString(object.hash_algorithm);
  wrapped.hashValue = MMXString(object.hash);
  wrapped.sizeBytes = static_cast<unsigned long long>(object.size_bytes);
  wrapped.filename = MMXString(object.filename);
  wrapped.createdAt = MMXString(object.created_at);
  wrapped.contentPath = MMXPathString(object.content_path);
  wrapped.objectDirectory = MMXPathString(object.object_dir);
  wrapped.metadataPath = MMXPathString(object.metadata_path);
  return wrapped;
}

MMXStoreFile *MMXWrapStoreFile(const memix::StoreFile &file) {
  MMXStoreFile *wrapped = [[MMXStoreFile alloc] init];
  wrapped.type = MMXObjectTypeFromMemory(file.type);
  wrapped.contextPath = MMXPathString(file.context_path);
  wrapped.blob = MMXWrapStoredObject(file.blob);
  return wrapped;
}

MMXViewArtifact *MMXWrapViewArtifact(const memix::ViewArtifact &artifact) {
  MMXViewArtifact *wrapped = [[MMXViewArtifact alloc] init];
  wrapped.derivationID = MMXString(artifact.derivation_id);
  wrapped.expressionID = MMXString(artifact.expression_id);
  wrapped.agentGeneration = MMXString(artifact.agent_generation);
  wrapped.inputBlobIDs = MMXStringArray(artifact.input_blob_ids);
  wrapped.manifestPath = MMXPathString(artifact.manifest_path);
  wrapped.treePath = MMXPathString(artifact.tree_path);
  wrapped.inserted = artifact.inserted;
  return wrapped;
}

std::expected<std::vector<memix::StoreFile>, memix::StoreError>
MMXCollectViewFiles(const memix::Store &store,
                    const memix::ViewArtifact &artifact) {
  std::vector<memix::StoreFile> files;

  std::error_code iterator_error;
  const std::filesystem::recursive_directory_iterator end;
  for (std::filesystem::recursive_directory_iterator iterator{
           artifact.tree_path, iterator_error};
       !iterator_error && iterator != end; iterator.increment(iterator_error)) {
    const std::filesystem::directory_entry &entry = *iterator;
    std::error_code status_error;
    const bool is_symlink =
        std::filesystem::is_symlink(entry.symlink_status(status_error));
    if (status_error) {
      return std::unexpected(memix::StoreError{
          .message = "failed to inspect view tree entry"});
    }
    if (!is_symlink) {
      continue;
    }

    std::error_code symlink_error;
    const std::filesystem::path target =
        std::filesystem::read_symlink(entry.path(), symlink_error);
    if (symlink_error) {
      return std::unexpected(memix::StoreError{
          .message = "failed to read view tree symlink"});
    }
    if (target.filename() != "object") {
      return std::unexpected(memix::StoreError{
          .message = "view tree symlink target is not a memix object"});
    }

    const std::string blob_id = target.parent_path().filename().string();
    const std::expected<memix::StoredObject, memix::StoreError> blob =
        store.read_blob(memix::MemoryObjectType::Agents, blob_id);
    if (!blob) {
      return std::unexpected(blob.error());
    }

    files.push_back(memix::StoreFile{
        .type = memix::MemoryObjectType::Agents,
        .context_path = entry.path().lexically_relative(artifact.tree_path),
        .blob = *blob,
    });
  }
  if (iterator_error) {
    return std::unexpected(memix::StoreError{
        .message = "failed to list view tree directory"});
  }

  std::ranges::sort(files, {}, &memix::StoreFile::context_path);
  return files;
}

} // namespace

@implementation MMXStore {
  std::filesystem::path _root;
  std::unique_ptr<memix::Store> _store;
}

+ (NSURL *)defaultStoreRootAndReturnError:(NSError **)error {
  const std::expected<std::filesystem::path, memix::StoreError> root =
      memix::default_store_root();
  if (!root) {
    MMXSetStoreError(error, root.error());
    return nil;
  }
  return [NSURL fileURLWithPath:MMXPathString(*root)];
}

- (instancetype)initWithRootURL:(NSURL *)rootURL error:(NSError **)error {
  self = [super init];
  if (self == nil) {
    return nil;
  }
  if (rootURL == nil || !rootURL.isFileURL) {
    MMXSetError(error, MMXMakeError(@"Store root must be a file URL."));
    return nil;
  }

  _root = MMXPath(rootURL);
  _store = std::make_unique<memix::Store>(_root);
  return self;
}

- (NSURL *)rootURL {
  return [NSURL fileURLWithPath:MMXPathString(_root)];
}

- (NSArray<MMXStoredObject *> *)listAllObjectsAndReturnError:(NSError **)error {
  const std::expected<std::vector<memix::StoredObject>, memix::StoreError> objects =
      _store->list(std::nullopt);
  if (!objects) {
    MMXSetStoreError(error, objects.error());
    return nil;
  }

  NSMutableArray<MMXStoredObject *> *wrapped =
      [[NSMutableArray alloc] initWithCapacity:objects->size()];
  for (const memix::StoredObject &object : *objects) {
    [wrapped addObject:MMXWrapStoredObject(object)];
  }
  return wrapped;
}

- (NSArray<MMXStoredObject *> *)listObjectsWithType:(MMXObjectType)type
                                             error:(NSError **)error {
  memix::MemoryObjectType memix_type = memix::MemoryObjectType::Agents;
  if (!MMXRequireType(type, &memix_type, error)) {
    return nil;
  }

  const std::expected<std::vector<memix::StoredObject>, memix::StoreError> objects =
      _store->list(memix_type);
  if (!objects) {
    MMXSetStoreError(error, objects.error());
    return nil;
  }

  NSMutableArray<MMXStoredObject *> *wrapped =
      [[NSMutableArray alloc] initWithCapacity:objects->size()];
  for (const memix::StoredObject &object : *objects) {
    [wrapped addObject:MMXWrapStoredObject(object)];
  }
  return wrapped;
}

- (NSArray<MMXStoreFile *> *)listFilesWithType:(MMXObjectType)type
                                    generation:(NSString *)generation
                                         error:(NSError **)error {
  memix::MemoryObjectType memix_type = memix::MemoryObjectType::Agents;
  if (!MMXRequireType(type, &memix_type, error)) {
    return nil;
  }

  const std::optional<std::string> generation_storage = MMXOptionalString(generation);
  const std::expected<std::vector<memix::StoreFile>, memix::StoreError> files =
      _store->list_files(memix_type, MMXOptionalStringView(generation_storage));
  if (!files) {
    MMXSetStoreError(error, files.error());
    return nil;
  }

  NSMutableArray<MMXStoreFile *> *wrapped =
      [[NSMutableArray alloc] initWithCapacity:files->size()];
  for (const memix::StoreFile &file : *files) {
    [wrapped addObject:MMXWrapStoreFile(file)];
  }
  return wrapped;
}

- (NSData *)readFileDataWithType:(MMXObjectType)type
                      contextPath:(NSString *)contextPath
                       generation:(NSString *)generation
                            error:(NSError **)error {
  memix::MemoryObjectType memix_type = memix::MemoryObjectType::Agents;
  if (!MMXRequireType(type, &memix_type, error)) {
    return nil;
  }

  const std::optional<std::string> generation_storage = MMXOptionalString(generation);
  const std::expected<memix::StoredObject, memix::StoreError> object =
      _store->read_file(memix_type, MMXPath(contextPath),
                        MMXOptionalStringView(generation_storage));
  if (!object) {
    MMXSetStoreError(error, object.error());
    return nil;
  }

  return MMXReadData(object->object_dir / object->content_path, error);
}

- (NSArray<MMXStoreFile *> *)listFilesInFileTreeViewWithDerivationID:
    (NSString *)derivationID error:(NSError **)error {
  memix::ViewService views{memix::Store(_root)};
  const std::expected<memix::ViewArtifact, memix::StoreError> artifact =
      views.resolve_file_tree(MMXStdString(derivationID));
  if (!artifact) {
    MMXSetStoreError(error, artifact.error());
    return nil;
  }

  const std::expected<std::vector<memix::StoreFile>, memix::StoreError> files =
      MMXCollectViewFiles(*_store, *artifact);
  if (!files) {
    MMXSetStoreError(error, files.error());
    return nil;
  }

  NSMutableArray<MMXStoreFile *> *wrapped =
      [[NSMutableArray alloc] initWithCapacity:files->size()];
  for (const memix::StoreFile &file : *files) {
    [wrapped addObject:MMXWrapStoreFile(file)];
  }
  return wrapped;
}

- (NSData *)readFileTreeViewDataWithDerivationID:(NSString *)derivationID
                                     contextPath:(NSString *)contextPath
                                           error:(NSError **)error {
  memix::ViewService views{memix::Store(_root)};
  const std::expected<memix::ViewArtifact, memix::StoreError> artifact =
      views.resolve_file_tree(MMXStdString(derivationID));
  if (!artifact) {
    MMXSetStoreError(error, artifact.error());
    return nil;
  }

  const std::filesystem::path relative_path = MMXPath(contextPath);
  if (!MMXPathIsSafeRelative(relative_path)) {
    MMXSetError(error, MMXMakeError(@"View path must be a relative file path."));
    return nil;
  }

  const std::filesystem::path file_path = artifact->tree_path / relative_path;
  std::error_code status_error;
  const bool is_symlink =
      std::filesystem::is_symlink(std::filesystem::symlink_status(file_path,
                                                                  status_error));
  if (status_error || !is_symlink) {
    MMXSetError(error, MMXMakeError(@"View file is not present in this view."));
    return nil;
  }

  return MMXReadData(file_path, error);
}

- (MMXIngestResult *)ingestFileAtURL:(NSURL *)sourceURL
                                 type:(MMXObjectType)type
                          contextPath:(NSString *)contextPath
                              message:(NSString *)message
                                error:(NSError **)error {
  memix::MemoryObjectType memix_type = memix::MemoryObjectType::Agents;
  if (!MMXRequireType(type, &memix_type, error)) {
    return nil;
  }

  const std::string message_storage = MMXStdString(message != nil ? message : @"");
  const std::expected<memix::FileIngestResult, memix::StoreError> result =
      _store->ingest_file(memix::FileIngestRequest{
          .type = memix_type,
          .source_path = MMXPath(sourceURL),
          .context_path = MMXPath(contextPath),
          .message = std::string_view(message_storage),
      });
  if (!result) {
    MMXSetStoreError(error, result.error());
    return nil;
  }

  MMXIngestResult *wrapped = [[MMXIngestResult alloc] init];
  wrapped.blobID = MMXString(result->blob.object.id);
  wrapped.generationID = MMXString(result->generation.id);
  wrapped.contextPath = MMXPathString(result->context_path);
  wrapped.inserted = result->blob.inserted;
  return wrapped;
}

- (NSArray<MMXViewArtifact *> *)listFileTreeViewsAndReturnError:
    (NSError **)error {
  memix::ViewService views{memix::Store(_root)};
  const std::expected<std::vector<memix::ViewArtifact>, memix::StoreError> artifacts =
      views.list_file_trees();
  if (!artifacts) {
    MMXSetStoreError(error, artifacts.error());
    return nil;
  }

  NSMutableArray<MMXViewArtifact *> *wrapped =
      [[NSMutableArray alloc] initWithCapacity:artifacts->size()];
  for (const memix::ViewArtifact &artifact : *artifacts) {
    [wrapped addObject:MMXWrapViewArtifact(artifact)];
  }
  return wrapped;
}

- (MMXViewArtifact *)buildFileTreeWithName:(NSString *)name
                        requiredAgentPaths:
                            (NSArray<NSString *> *)requiredAgentPaths
                        optionalAgentPaths:
                            (NSArray<NSString *> *)optionalAgentPaths
                                generation:(NSString *)generation
                                       ref:(NSString *)ref
                                     error:(NSError **)error {
  memix::ViewExpression expression;
  expression.name = MMXStdString(name.length == 0 ? @"file-tree" : name);

  if (generation.length > 0 && ref.length > 0) {
    MMXSetError(error, MMXMakeError(@"Use either generation or ref, not both."));
    return nil;
  }
  if (generation.length > 0) {
    expression.agent_generation = MMXStdString(generation);
  } else if (ref.length > 0) {
    expression.agent_ref = MMXStdString(ref);
  }

  expression.selectors.reserve(requiredAgentPaths.count + optionalAgentPaths.count);
  for (NSString *path in requiredAgentPaths) {
    expression.selectors.push_back(memix::ViewSelector{
        .agent_path = MMXPath(path),
        .scope_label = std::nullopt,
        .required = true,
    });
  }
  for (NSString *path in optionalAgentPaths) {
    expression.selectors.push_back(memix::ViewSelector{
        .agent_path = MMXPath(path),
        .scope_label = std::nullopt,
        .required = false,
    });
  }

  memix::ViewService views{memix::Store(_root)};
  const std::expected<memix::ViewArtifact, memix::StoreError> artifact =
      views.build_file_tree(expression);
  if (!artifact) {
    MMXSetStoreError(error, artifact.error());
    return nil;
  }
  return MMXWrapViewArtifact(*artifact);
}

@end
