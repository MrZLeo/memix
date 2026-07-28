#import "MMXStore.h"

#include "memix/error.hpp"
#include "memix/store.hpp"
#include "memix/view.hpp"

#include <algorithm>
#include <cstdint>
#include <expected>
#include <format>
#include <fstream>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace fs = memix::fs;

namespace {

NSString *const MMXErrorDomain = @"com.memix.inspector";
constexpr std::size_t kSha256HexLength = 64;

NSString *MMXString(std::string_view value);
NSString *MMXString(const std::string &value);

NSError *MMXMakeError(NSString *message) {
  NSString *const description = message != nil ? message : @"Memix error";
  return [NSError errorWithDomain:MMXErrorDomain
                             code:1
                         userInfo:@{NSLocalizedDescriptionKey : description}];
}

NSError *MMXMakeError(const memix::StoreError &error) {
  return MMXMakeError(MMXString(error.message));
}

NSError *MMXMakeError(const memix::ViewError &error) {
  return MMXMakeError(MMXString(error.message));
}

memix::FsError MMXMakeFilesystemError(std::string_view operation,
                                      const std::error_code &error,
                                      std::vector<fs::path> paths) {
  std::string message{operation};
  std::string_view separator = " `";
  for (const fs::path &path : paths) {
    message += std::format("{}{}`", separator, path.string());
    separator = " to `";
  }
  message = std::format("{}: {}", message, error.message());
  return memix::FsError{
      .message = std::move(message),
      .code = error,
      .operation = std::string(operation),
      .paths = std::move(paths),
  };
}

memix::ViewError MMXMakeViewError(memix::FsError error) {
  return memix::ViewError{
      .message = error.message,
      .filesystem_error = std::move(error),
      .kind = memix::ErrorKind::Filesystem,
  };
}

memix::ViewError MMXMakeViewError(memix::ErrorKind kind, std::string message) {
  return memix::ViewError{
      .message = std::move(message),
      .filesystem_error = std::nullopt,
      .kind = kind,
  };
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

bool MMXSetViewError(NSError **out_error, const memix::ViewError &error) {
  return MMXSetError(out_error, MMXMakeError(error));
}

NSString *MMXString(std::string_view value) {
  NSString *const string =
      [[NSString alloc] initWithBytes:value.data()
                               length:value.size()
                             encoding:NSUTF8StringEncoding];
  return string != nil ? string : @"";
}

NSString *MMXString(const std::string &value) {
  return MMXString(std::string_view(value));
}

NSString *MMXPathString(const fs::path &path) {
  return MMXString(path.generic_string());
}

std::string MMXStdString(NSString *value) {
  if (value == nil) {
    return {};
  }
  const char *const utf8 = value.UTF8String;
  return utf8 == nullptr ? std::string() : std::string(utf8);
}

fs::path MMXPath(NSString *value) { return {MMXStdString(value)}; }

fs::path MMXPath(NSURL *url) { return {MMXStdString(url.path)}; }

MMXObjectType MMXObjectTypeFromMemory(memix::MemoryObjectType type) {
  switch (type) {
  case memix::MemoryObjectType::Rules:
    return MMXObjectTypeRules;
  case memix::MemoryObjectType::Chat:
    return MMXObjectTypeChat;
  case memix::MemoryObjectType::Documents:
    return MMXObjectTypeDocuments;
  }
}

std::optional<memix::MemoryObjectType> MMXMemoryObjectType(MMXObjectType type) {
  switch (type) {
  case MMXObjectTypeRules:
    return memix::MemoryObjectType::Rules;
  case MMXObjectTypeChat:
    return memix::MemoryObjectType::Chat;
  case MMXObjectTypeDocuments:
    return memix::MemoryObjectType::Documents;
  }
  return std::nullopt;
}

bool MMXRequireType(MMXObjectType type, memix::MemoryObjectType *out_type,
                    NSError **out_error) {
  const std::optional<memix::MemoryObjectType> parsed =
      MMXMemoryObjectType(type);
  if (!parsed.has_value()) {
    return MMXSetError(out_error,
                       MMXMakeError(@"Unsupported memix object type."));
  }
  *out_type = *parsed;
  return true;
}

MMXViewTargetKind MMXViewTargetKindFromMemory(memix::ViewTargetKind target) {
  switch (target) {
  case memix::ViewTargetKind::FileTree:
    return MMXViewTargetKindFileTree;
  case memix::ViewTargetKind::ContextPack:
    return MMXViewTargetKindContextPack;
  }
}

std::optional<memix::ViewTargetKind>
MMXViewTargetKindFromBridge(MMXViewTargetKind target) {
  switch (target) {
  case MMXViewTargetKindFileTree:
    return memix::ViewTargetKind::FileTree;
  case MMXViewTargetKindContextPack:
    return memix::ViewTargetKind::ContextPack;
  }
  return std::nullopt;
}

bool MMXRequireTarget(MMXViewTargetKind target,
                      memix::ViewTargetKind *out_target, NSError **out_error) {
  const std::optional<memix::ViewTargetKind> parsed =
      MMXViewTargetKindFromBridge(target);
  if (!parsed.has_value()) {
    return MMXSetError(out_error,
                       MMXMakeError(@"Unsupported memix view target."));
  }
  *out_target = *parsed;
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
  NSMutableArray<NSString *> *const items =
      [[NSMutableArray alloc] initWithCapacity:values.size()];
  for (const std::string &value : values) {
    [items addObject:MMXString(value)];
  }
  return items;
}

std::vector<std::string> MMXStringVector(NSArray<NSString *> *values) {
  std::vector<std::string> output;
  output.reserve(values.count);
  for (NSUInteger index = 0; index < values.count; ++index) {
    NSString *const value = values[index];
    output.push_back(MMXStdString(value));
  }
  return output;
}

NSData *MMXReadData(const fs::path &path, NSError **out_error) {
  NSURL *const url = [NSURL fileURLWithPath:MMXPathString(path)];
  NSError *read_error = nil;
  NSData *const data = [NSData dataWithContentsOfURL:url
                                             options:NSDataReadingMappedIfSafe
                                               error:&read_error];
  if (data == nil) {
    MMXSetError(out_error, read_error != nil
                               ? read_error
                               : MMXMakeError(@"Failed to read file data."));
  }
  return data;
}

bool MMXPathIsSafeRelative(const fs::path &path) {
  if (path.empty() || path.is_absolute()) {
    return false;
  }
  return std::ranges::all_of(path, [](const fs::path &component) {
    return !component.empty() && component != "." && component != "..";
  });
}

bool MMXIsLowerHexSha256(std::string_view value) {
  if (value.size() != kSha256HexLength) {
    return false;
  }
  return std::ranges::all_of(value, [](char character) {
    return (character >= '0' && character <= '9')
        || (character >= 'a' && character <= 'f');
  });
}

fs::path MMXViewEnvironmentsDirectory(const fs::path &root) {
  return root / "views" / "environments";
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

@interface MMXResolvedViewFile ()
@property(nonatomic, copy, readwrite) NSString *locator;
@property(nonatomic, readwrite) MMXObjectType type;
@property(nonatomic, copy, readwrite) NSString *sourcePath;
@property(nonatomic, copy, readwrite) NSString *sourceGeneration;
@property(nonatomic, copy, readwrite) NSString *outputPath;
@property(nonatomic, strong, readwrite) MMXStoredObject *blob;
@end

@implementation MMXResolvedViewFile
@end

@interface MMXViewDerivationOutput ()
@property(nonatomic, copy, readwrite) NSString *role;
@property(nonatomic, copy, readwrite) NSString *artifact;
@property(nonatomic, copy, readwrite) NSString *path;
@end

@implementation MMXViewDerivationOutput
@end

@interface MMXViewEnvironmentClosure ()
@property(nonatomic, copy, readwrite) NSArray<NSString *> *derivations;
@property(nonatomic, copy, readwrite) NSArray<NSString *> *artifacts;
@property(nonatomic, copy, readwrite) NSArray<NSString *> *rawObjects;
@property(nonatomic, copy, readwrite) NSArray<NSString *> *environments;
@end

@implementation MMXViewEnvironmentClosure
@end

@interface MMXResolvedViewEnvironmentInput ()
@property(nonatomic, copy, readwrite) NSString *environmentID;
@property(nonatomic, copy, readwrite) NSString *derivationID;
@property(nonatomic, copy, readwrite) NSString *artifactID;
@property(nonatomic, copy, readwrite) NSString *outputRole;
@property(nonatomic, copy, readwrite) NSString *mount;
@property(nonatomic, strong, readwrite) MMXViewEnvironmentClosure *closure;
@end

@implementation MMXResolvedViewEnvironmentInput
@end

@interface MMXViewDerivation ()
@property(nonatomic, copy, readwrite) NSString *derivationID;
@property(nonatomic, copy, readwrite) NSString *irHash;
@property(nonatomic, readwrite) MMXViewTargetKind target;
@property(nonatomic, copy, readwrite)
    NSArray<MMXResolvedViewFile *> *resolvedFiles;
@property(nonatomic, copy, readwrite)
    NSArray<MMXViewDerivationOutput *> *outputs;
@property(nonatomic, copy, readwrite)
    NSArray<MMXResolvedViewEnvironmentInput *> *environmentInputs;
@property(nonatomic, copy, readwrite) NSString *manifestPath;
@property(nonatomic, copy, readwrite) NSString *treePath;
@property(nonatomic, readwrite) BOOL inserted;
@end

@implementation MMXViewDerivation
@end

@interface MMXViewEnvironmentBinding ()
@property(nonatomic, copy, readwrite) NSString *mount;
@property(nonatomic, copy, readwrite) NSString *derivationID;
@property(nonatomic, copy, readwrite) NSString *outputRole;
@end

@implementation MMXViewEnvironmentBinding
@end

@interface MMXViewEnvironment ()
@property(nonatomic, copy, readwrite) NSString *environmentID;
@property(nonatomic, copy, readwrite) NSString *manifestPath;
@property(nonatomic, copy, readwrite)
    NSArray<MMXViewEnvironmentBinding *> *bindings;
@property(nonatomic, strong, readwrite) MMXViewEnvironmentClosure *closure;
@property(nonatomic, strong, readwrite) MMXViewDerivation *derivation;
@property(nonatomic, readwrite) BOOL inserted;
@end

@implementation MMXViewEnvironment
@end

namespace {

MMXStoredObject *MMXWrapStoredObject(const memix::StoredObject &object) {
  MMXStoredObject *const wrapped = [[MMXStoredObject alloc] init];
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
  MMXStoreFile *const wrapped = [[MMXStoreFile alloc] init];
  wrapped.type = MMXObjectTypeFromMemory(file.type);
  wrapped.contextPath = MMXPathString(file.context_path);
  wrapped.blob = MMXWrapStoredObject(file.blob);
  return wrapped;
}

MMXResolvedViewFile *
MMXWrapResolvedViewFile(const memix::ResolvedViewFile &file) {
  MMXResolvedViewFile *const wrapped = [[MMXResolvedViewFile alloc] init];
  wrapped.locator = MMXString(file.locator);
  wrapped.type = MMXObjectTypeFromMemory(file.type);
  wrapped.sourcePath = MMXPathString(file.source_path);
  wrapped.sourceGeneration = MMXString(file.source_generation);
  wrapped.outputPath = MMXPathString(file.output_path);
  wrapped.blob = MMXWrapStoredObject(file.blob);
  return wrapped;
}

NSArray<MMXResolvedViewFile *> *
MMXWrapResolvedViewFiles(const std::vector<memix::ResolvedViewFile> &files) {
  NSMutableArray<MMXResolvedViewFile *> *const wrapped =
      [[NSMutableArray alloc] initWithCapacity:files.size()];
  for (const memix::ResolvedViewFile &file : files) {
    [wrapped addObject:MMXWrapResolvedViewFile(file)];
  }
  return wrapped;
}

MMXViewDerivationOutput *
MMXWrapViewDerivationOutput(const memix::ViewDerivationOutput &output) {
  MMXViewDerivationOutput *const wrapped =
      [[MMXViewDerivationOutput alloc] init];
  wrapped.role = MMXString(output.role);
  wrapped.artifact = MMXString(output.artifact);
  wrapped.path = MMXPathString(output.path);
  return wrapped;
}

NSArray<MMXViewDerivationOutput *> *MMXWrapViewDerivationOutputs(
    const std::vector<memix::ViewDerivationOutput> &outputs) {
  NSMutableArray<MMXViewDerivationOutput *> *const wrapped =
      [[NSMutableArray alloc] initWithCapacity:outputs.size()];
  for (const memix::ViewDerivationOutput &output : outputs) {
    [wrapped addObject:MMXWrapViewDerivationOutput(output)];
  }
  return wrapped;
}

MMXViewEnvironmentClosure *
MMXWrapViewEnvironmentClosure(const memix::ViewEnvironmentClosure &closure) {
  MMXViewEnvironmentClosure *const wrapped =
      [[MMXViewEnvironmentClosure alloc] init];
  wrapped.derivations = MMXStringArray(closure.derivations);
  wrapped.artifacts = MMXStringArray(closure.artifacts);
  wrapped.rawObjects = MMXStringArray(closure.raw_objects);
  wrapped.environments = MMXStringArray(closure.environments);
  return wrapped;
}

MMXResolvedViewEnvironmentInput *MMXWrapResolvedViewEnvironmentInput(
    const memix::ResolvedViewEnvironmentInput &input) {
  MMXResolvedViewEnvironmentInput *const wrapped =
      [[MMXResolvedViewEnvironmentInput alloc] init];
  wrapped.environmentID = MMXString(input.environment_id);
  wrapped.derivationID = MMXString(input.derivation_id);
  wrapped.artifactID = MMXString(input.artifact_id);
  wrapped.outputRole = MMXString(input.output_role);
  wrapped.mount = MMXPathString(input.mount);
  wrapped.closure = MMXWrapViewEnvironmentClosure(input.closure);
  return wrapped;
}

NSArray<MMXResolvedViewEnvironmentInput *> *
MMXWrapResolvedViewEnvironmentInputs(
    const std::vector<memix::ResolvedViewEnvironmentInput> &inputs) {
  NSMutableArray<MMXResolvedViewEnvironmentInput *> *const wrapped =
      [[NSMutableArray alloc] initWithCapacity:inputs.size()];
  for (const memix::ResolvedViewEnvironmentInput &input : inputs) {
    [wrapped addObject:MMXWrapResolvedViewEnvironmentInput(input)];
  }
  return wrapped;
}

MMXViewDerivation *
MMXWrapViewDerivation(const memix::ViewDerivation &derivation) {
  MMXViewDerivation *const wrapped = [[MMXViewDerivation alloc] init];
  wrapped.derivationID = MMXString(derivation.id);
  wrapped.irHash = MMXString(derivation.ir_hash);
  wrapped.target = MMXViewTargetKindFromMemory(derivation.target);
  wrapped.resolvedFiles = MMXWrapResolvedViewFiles(derivation.resolved_files);
  wrapped.outputs = MMXWrapViewDerivationOutputs(derivation.outputs);
  wrapped.environmentInputs =
      MMXWrapResolvedViewEnvironmentInputs(derivation.environment_inputs);
  wrapped.manifestPath = MMXPathString(derivation.manifest_path);
  wrapped.treePath = MMXPathString(derivation.tree_path);
  wrapped.inserted = derivation.inserted;
  return wrapped;
}

MMXViewEnvironmentBinding *
MMXWrapViewEnvironmentBinding(const memix::ViewEnvironmentBinding &binding) {
  MMXViewEnvironmentBinding *const wrapped =
      [[MMXViewEnvironmentBinding alloc] init];
  wrapped.mount = MMXPathString(binding.mount);
  wrapped.derivationID = MMXString(binding.derivation_id);
  wrapped.outputRole = MMXString(binding.output_role);
  return wrapped;
}

NSArray<MMXViewEnvironmentBinding *> *MMXWrapViewEnvironmentBindings(
    const std::vector<memix::ViewEnvironmentBinding> &bindings) {
  NSMutableArray<MMXViewEnvironmentBinding *> *const wrapped =
      [[NSMutableArray alloc] initWithCapacity:bindings.size()];
  for (const memix::ViewEnvironmentBinding &binding : bindings) {
    [wrapped addObject:MMXWrapViewEnvironmentBinding(binding)];
  }
  return wrapped;
}

MMXViewEnvironment *
MMXWrapViewEnvironment(const memix::ViewEnvironment &environment) {
  MMXViewEnvironment *const wrapped = [[MMXViewEnvironment alloc] init];
  wrapped.environmentID = MMXString(environment.id);
  wrapped.manifestPath = MMXPathString(environment.manifest_path);
  wrapped.bindings = MMXWrapViewEnvironmentBindings(environment.bindings);
  wrapped.closure = MMXWrapViewEnvironmentClosure(environment.closure);
  wrapped.derivation = MMXWrapViewDerivation(environment.derivation);
  wrapped.inserted = environment.inserted;
  return wrapped;
}

memix::ViewResult<std::vector<memix::ViewEnvironment>>
MMXListViewEnvironments(const fs::path &root) {
  const fs::path environments_dir = MMXViewEnvironmentsDirectory(root);
  const memix::FsResult<bool> exists = fs::exists(environments_dir);
  if (!exists) {
    return std::unexpected(MMXMakeViewError(exists.error()));
  }
  if (!*exists) {
    return std::vector<memix::ViewEnvironment>{};
  }

  const memix::FsResult<bool> is_directory = fs::is_directory(environments_dir);
  if (!is_directory) {
    return std::unexpected(MMXMakeViewError(is_directory.error()));
  }
  if (!*is_directory) {
    return std::unexpected(MMXMakeViewError(
        memix::ErrorKind::InvalidData,
        std::format("view environments path is not a directory `{}`",
                    environments_dir.string())));
  }

  const memix::ViewService views{memix::Store(root)};
  std::vector<memix::ViewEnvironment> environments;
  std::error_code iterator_error;
  const fs::directory_iterator end;
  for (fs::directory_iterator iterator{environments_dir, iterator_error};
       !iterator_error && iterator != end; iterator.increment(iterator_error)) {
    const fs::directory_entry &entry = *iterator;
    std::error_code entry_error;
    if (!entry.is_regular_file(entry_error)) {
      if (entry_error) {
        return std::unexpected(MMXMakeViewError(
            MMXMakeFilesystemError("failed to inspect view environment",
                                   entry_error, {entry.path()})));
      }
      continue;
    }

    const fs::path filename = entry.path().filename();
    if (filename.extension() != ".json") {
      continue;
    }

    const std::string environment_id = filename.stem().string();
    if (!MMXIsLowerHexSha256(environment_id)) {
      continue;
    }

    memix::ViewResult<memix::ViewEnvironment> environment =
        views.resolve_environment(environment_id);
    if (!environment) {
      return std::unexpected(std::move(environment.error()));
    }
    environments.push_back(std::move(*environment));
  }
  if (iterator_error) {
    return std::unexpected(MMXMakeViewError(
        MMXMakeFilesystemError("failed to list view environments",
                               iterator_error, {environments_dir})));
  }

  std::ranges::sort(environments, {}, &memix::ViewEnvironment::id);
  return environments;
}

memix::ViewResult<memix::ViewEnvironment>
MMXResolveViewEnvironment(const fs::path &root, NSString *environmentID) {
  const std::string id = MMXStdString(environmentID);
  if (id.empty()) {
    return std::unexpected(MMXMakeViewError(
        memix::ErrorKind::InvalidArgument, "environment id must not be empty"));
  }

  const memix::ViewService views{memix::Store(root)};
  return views.resolve_environment(id);
}

} // namespace

@implementation MMXStore {
  fs::path _root;
  std::unique_ptr<memix::Store> _store;
}

+ (NSURL *)defaultStoreRootAndReturnError:(NSError **)error {
  const std::expected<fs::path, memix::StoreError> root =
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
  const std::expected<std::vector<memix::StoredObject>, memix::StoreError>
      objects = _store->list(std::nullopt);
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
  memix::MemoryObjectType memix_type = memix::MemoryObjectType::Rules;
  if (!MMXRequireType(type, &memix_type, error)) {
    return nil;
  }

  const std::expected<std::vector<memix::StoredObject>, memix::StoreError>
      objects = _store->list(memix_type);
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
  memix::MemoryObjectType memix_type = memix::MemoryObjectType::Rules;
  if (!MMXRequireType(type, &memix_type, error)) {
    return nil;
  }

  const std::optional<std::string> generation_storage =
      MMXOptionalString(generation);
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
  memix::MemoryObjectType memix_type = memix::MemoryObjectType::Rules;
  if (!MMXRequireType(type, &memix_type, error)) {
    return nil;
  }

  const std::optional<std::string> generation_storage =
      MMXOptionalString(generation);
  const std::expected<memix::StoredObject, memix::StoreError> object =
      _store->read_file(memix_type, MMXPath(contextPath),
                        MMXOptionalStringView(generation_storage));
  if (!object) {
    MMXSetStoreError(error, object.error());
    return nil;
  }

  return MMXReadData(object->object_dir / object->content_path, error);
}

- (NSData *)readFileTreeViewDataWithEnvironmentID:(NSString *)environmentID
                                       outputPath:(NSString *)outputPath
                                            error:(NSError **)error {
  const memix::ViewResult<memix::ViewEnvironment> environment =
      MMXResolveViewEnvironment(_root, environmentID);
  if (!environment) {
    MMXSetViewError(error, environment.error());
    return nil;
  }

  const fs::path relative_path = MMXPath(outputPath);
  if (!MMXPathIsSafeRelative(relative_path)) {
    MMXSetError(error,
                MMXMakeError(@"View path must be a relative file path."));
    return nil;
  }

  const fs::path file_path = environment->derivation.tree_path / relative_path;
  const memix::FsResult<fs::file_status> status = fs::symlink_status(file_path);
  if (!status) {
    MMXSetViewError(error, MMXMakeViewError(status.error()));
    return nil;
  }
  const bool is_symlink = status->type() == fs::file_type::symlink;
  if (!is_symlink) {
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
  memix::MemoryObjectType memix_type = memix::MemoryObjectType::Rules;
  if (!MMXRequireType(type, &memix_type, error)) {
    return nil;
  }

  const std::string message_storage =
      MMXStdString(message != nil ? message : @"");
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

- (NSArray<MMXViewEnvironment *> *)listViewEnvironmentsAndReturnError:
    (NSError **)error {
  const memix::ViewResult<std::vector<memix::ViewEnvironment>> environments =
      MMXListViewEnvironments(_root);
  if (!environments) {
    MMXSetViewError(error, environments.error());
    return nil;
  }

  NSMutableArray<MMXViewEnvironment *> *wrapped =
      [[NSMutableArray alloc] initWithCapacity:environments->size()];
  for (const memix::ViewEnvironment &environment : *environments) {
    [wrapped addObject:MMXWrapViewEnvironment(environment)];
  }
  return wrapped;
}

- (MMXViewEnvironment *)resolveViewEnvironmentWithID:(NSString *)environmentID
                                               error:(NSError **)error {
  const memix::ViewResult<memix::ViewEnvironment> environment =
      MMXResolveViewEnvironment(_root, environmentID);
  if (!environment) {
    MMXSetViewError(error, environment.error());
    return nil;
  }
  return MMXWrapViewEnvironment(*environment);
}

- (MMXViewEnvironment *)
    buildViewEnvironmentWithSources:(NSArray<NSString *> *)sources
                             target:(MMXViewTargetKind)target
                              error:(NSError **)error {
  memix::ViewTargetKind memix_target = memix::ViewTargetKind::FileTree;
  if (!MMXRequireTarget(target, &memix_target, error)) {
    return nil;
  }

  memix::ViewService views{memix::Store(_root)};
  const memix::ViewResult<memix::AuthoredViewExpression> expression =
      views.expression_from_sources(MMXStringVector(sources), memix_target);
  if (!expression) {
    MMXSetViewError(error, expression.error());
    return nil;
  }
  const memix::ViewResult<memix::ViewEnvironment> environment =
      views.build(*expression);
  if (!environment) {
    MMXSetViewError(error, environment.error());
    return nil;
  }
  return MMXWrapViewEnvironment(*environment);
}

@end
