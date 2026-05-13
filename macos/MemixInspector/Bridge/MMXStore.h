#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

typedef NS_ENUM(NSInteger, MMXObjectType) {
  MMXObjectTypeAgents = 0,
  MMXObjectTypeChat = 1,
  MMXObjectTypeDocuments = 2,
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

@interface MMXViewArtifact : NSObject
@property(nonatomic, copy, readonly) NSString *derivationID;
@property(nonatomic, copy, readonly) NSString *expressionID;
@property(nonatomic, copy, readonly) NSString *agentGeneration;
@property(nonatomic, copy, readonly) NSArray<NSString *> *inputBlobIDs;
@property(nonatomic, copy, readonly) NSString *manifestPath;
@property(nonatomic, copy, readonly) NSString *treePath;
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

- (nullable NSArray<MMXStoredObject *> *)listObjectsWithType:
    (MMXObjectType)type error:(NSError **)error
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

- (nullable NSArray<MMXStoreFile *> *)listFilesInFileTreeViewWithDerivationID:
    (NSString *)derivationID error:(NSError **)error
    NS_SWIFT_NAME(listFilesInFileTreeView(derivationID:));

- (nullable NSData *)readFileTreeViewDataWithDerivationID:
    (NSString *)derivationID contextPath:(NSString *)contextPath
                                    error:(NSError **)error
    NS_SWIFT_NAME(readFileTreeViewData(derivationID:contextPath:));

- (nullable MMXIngestResult *)ingestFileAtURL:(NSURL *)sourceURL
                                         type:(MMXObjectType)type
                                  contextPath:(NSString *)contextPath
                                      message:(nullable NSString *)message
                                        error:(NSError **)error
    NS_SWIFT_NAME(ingestFile(at:type:contextPath:message:));

- (nullable NSArray<MMXViewArtifact *> *)listFileTreeViewsAndReturnError:
    (NSError **)error NS_SWIFT_NAME(listFileTreeViews());

- (nullable MMXViewArtifact *)buildFileTreeWithName:(NSString *)name
                                 requiredAgentPaths:
                                     (NSArray<NSString *> *)requiredAgentPaths
                                 optionalAgentPaths:
                                     (NSArray<NSString *> *)optionalAgentPaths
                                         generation:
                                             (nullable NSString *)generation
                                                ref:(nullable NSString *)ref
                                              error:(NSError **)error
    NS_SWIFT_NAME(buildFileTree(name:requiredAgentPaths:optionalAgentPaths:generation:ref:));

@end

NS_ASSUME_NONNULL_END
