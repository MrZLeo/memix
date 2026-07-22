#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

typedef NS_ENUM(NSInteger, MMXObjectType) {
  MMXObjectTypeRules = 0,
  MMXObjectTypeChat = 1,
  MMXObjectTypeDocuments = 2,
};

typedef NS_ENUM(NSInteger, MMXViewTargetKind) {
  MMXViewTargetKindFileTree = 0,
  MMXViewTargetKindContextPack = 1,
};

@interface MMXStoredObject : NSObject
@property(nonatomic, readonly) NSInteger schemaVersion;
@property(nonatomic, readonly) NSInteger objectVersion;
@property(nonatomic, readonly) MMXObjectType type;
@property(nonatomic, copy, readonly) NSString *objectID;
@property(nonatomic, copy, readonly) NSString *hashAlgorithm;
@property(nonatomic, copy, readonly) NSString *hashValue;
@property(nonatomic, readonly) unsigned long long sizeBytes;
@property(nonatomic, copy, readonly) NSString *filename;
@property(nonatomic, copy, readonly) NSString *createdAt;
@property(nonatomic, copy, readonly) NSString *contentPath;
@property(nonatomic, copy, readonly) NSString *objectDirectory;
@property(nonatomic, copy, readonly) NSString *metadataPath;
@end

@interface MMXStoreFile : NSObject
@property(nonatomic, readonly) MMXObjectType type;
@property(nonatomic, copy, readonly) NSString *contextPath;
@property(nonatomic, strong, readonly) MMXStoredObject *blob;
@end

@interface MMXIngestResult : NSObject
@property(nonatomic, copy, readonly) NSString *blobID;
@property(nonatomic, copy, readonly) NSString *generationID;
@property(nonatomic, copy, readonly) NSString *contextPath;
@property(nonatomic, readonly) BOOL inserted;
@end

@interface MMXResolvedViewFile : NSObject
@property(nonatomic, copy, readonly) NSString *locator;
@property(nonatomic, readonly) MMXObjectType type;
@property(nonatomic, copy, readonly) NSString *sourcePath;
@property(nonatomic, copy, readonly) NSString *sourceGeneration;
@property(nonatomic, copy, readonly) NSString *outputPath;
@property(nonatomic, strong, readonly) MMXStoredObject *blob;
@end

@interface MMXViewDerivationOutput : NSObject
@property(nonatomic, copy, readonly) NSString *role;
@property(nonatomic, copy, readonly) NSString *artifact;
@property(nonatomic, copy, readonly) NSString *path;
@end

@interface MMXViewDerivation : NSObject
@property(nonatomic, copy, readonly) NSString *derivationID;
@property(nonatomic, copy, readonly) NSString *irHash;
@property(nonatomic, readonly) MMXViewTargetKind target;
@property(nonatomic, copy, readonly)
    NSArray<MMXResolvedViewFile *> *resolvedFiles;
@property(nonatomic, copy, readonly)
    NSArray<MMXViewDerivationOutput *> *outputs;
@property(nonatomic, copy, readonly) NSString *manifestPath;
@property(nonatomic, copy, readonly) NSString *treePath;
@property(nonatomic, readonly) BOOL inserted;
@end

@interface MMXViewEnvironmentBinding : NSObject
@property(nonatomic, copy, readonly) NSString *mount;
@property(nonatomic, copy, readonly) NSString *derivationID;
@property(nonatomic, copy, readonly) NSString *outputRole;
@end

@interface MMXViewEnvironmentClosure : NSObject
@property(nonatomic, copy, readonly) NSArray<NSString *> *derivations;
@property(nonatomic, copy, readonly) NSArray<NSString *> *artifacts;
@property(nonatomic, copy, readonly) NSArray<NSString *> *rawObjects;
@end

@interface MMXViewEnvironment : NSObject
@property(nonatomic, copy, readonly) NSString *environmentID;
@property(nonatomic, copy, readonly) NSString *manifestPath;
@property(nonatomic, copy, readonly)
    NSArray<MMXViewEnvironmentBinding *> *bindings;
@property(nonatomic, strong, readonly) MMXViewEnvironmentClosure *closure;
@property(nonatomic, strong, readonly) MMXViewDerivation *derivation;
@property(nonatomic, readonly) BOOL inserted;
@end

@interface MMXStore : NSObject

+ (nullable NSURL *)defaultStoreRootAndReturnError:(NSError **)error
    NS_SWIFT_NAME(defaultStoreRoot());

- (nullable instancetype)initWithRootURL:(NSURL *)rootURL
                                   error:(NSError **)error
    NS_SWIFT_NAME(init(rootURL:));

- (instancetype)init NS_UNAVAILABLE;

@property(nonatomic, strong, readonly) NSURL *rootURL;

- (nullable NSArray<MMXStoredObject *> *)listAllObjectsAndReturnError:
    (NSError **)error NS_SWIFT_NAME(listAllObjects());

- (nullable NSArray<MMXStoredObject *> *)listObjectsWithType:(MMXObjectType)type
                                                       error:(NSError **)error
    NS_SWIFT_NAME(listObjects(type:));

- (nullable NSArray<MMXStoreFile *> *)listFilesWithType:(MMXObjectType)type
                                             generation:
                                                 (nullable NSString *)generation
                                                  error:(NSError **)error
    NS_SWIFT_NAME(listFiles(type:generation:));

- (nullable NSData *)readFileDataWithType:(MMXObjectType)type
                              contextPath:(NSString *)contextPath
                               generation:(nullable NSString *)generation
                                    error:(NSError **)error
    NS_SWIFT_NAME(readFileData(type:contextPath:generation:));

- (nullable NSData *)
    readFileTreeViewDataWithEnvironmentID:(NSString *)environmentID
                               outputPath:(NSString *)outputPath
                                    error:(NSError **)error
    NS_SWIFT_NAME(readFileTreeViewData(environmentID:outputPath:));

- (nullable MMXIngestResult *)ingestFileAtURL:(NSURL *)sourceURL
                                         type:(MMXObjectType)type
                                  contextPath:(NSString *)contextPath
                                      message:(nullable NSString *)message
                                        error:(NSError **)error
    NS_SWIFT_NAME(ingestFile(at:type:contextPath:message:));

- (nullable NSArray<MMXViewEnvironment *> *)listViewEnvironmentsAndReturnError:
    (NSError **)error NS_SWIFT_NAME(listViewEnvironments());

- (nullable MMXViewEnvironment *)resolveViewEnvironmentWithID:
                                     (NSString *)environmentID
                                                        error:(NSError **)error
    NS_SWIFT_NAME(resolveViewEnvironment(id:));

- (nullable MMXViewEnvironment *)
    buildViewEnvironmentWithSources:(NSArray<NSString *> *)sources
                             target:(MMXViewTargetKind)target
                              error:(NSError **)error
    NS_SWIFT_NAME(buildViewEnvironment(sources:target:));

@end

NS_ASSUME_NONNULL_END
