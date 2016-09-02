/*
 * Copyright (C) 2015 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "AppInfo.h"
#include "Debug.h"
#include "Flags.h"
#include "Locale.h"
#include "NameMangler.h"
#include "ResourceUtils.h"
#include "compile/IdAssigner.h"
#include "filter/ConfigFilter.h"
#include "flatten/Archive.h"
#include "flatten/TableFlattener.h"
#include "flatten/XmlFlattener.h"
#include "io/FileSystem.h"
#include "io/ZipArchive.h"
#include "java/JavaClassGenerator.h"
#include "java/ManifestClassGenerator.h"
#include "java/ProguardRules.h"
#include "link/Linkers.h"
#include "link/ProductFilter.h"
#include "link/ReferenceLinker.h"
#include "link/ManifestFixer.h"
#include "link/TableMerger.h"
#include "process/IResourceTableConsumer.h"
#include "process/SymbolTable.h"
#include "proto/ProtoSerialize.h"
#include "split/TableSplitter.h"
#include "unflatten/BinaryResourceParser.h"
#include "util/Files.h"
#include "util/StringPiece.h"
#include "xml/XmlDom.h"

#include <android-base/file.h>
#include <google/protobuf/io/coded_stream.h>

#include <fstream>
#include <queue>
#include <sys/stat.h>
#include <unordered_map>
#include <vector>

using google::protobuf::io::CopyingOutputStreamAdaptor;

namespace aapt {

struct LinkOptions {
    std::string outputPath;
    std::string manifestPath;
    std::vector<std::string> includePaths;
    std::vector<std::string> overlayFiles;

    // Java/Proguard options.
    Maybe<std::string> generateJavaClassPath;
    Maybe<std::string> customJavaPackage;
    std::set<std::string> extraJavaPackages;
    Maybe<std::string> generateProguardRulesPath;
    Maybe<std::string> generateMainDexProguardRulesPath;

    bool noAutoVersion = false;
    bool noVersionVectors = false;
    bool staticLib = false;
    bool noStaticLibPackages = false;
    bool generateNonFinalIds = false;
    std::vector<std::string> javadocAnnotations;
    bool outputToDirectory = false;
    bool noXmlNamespaces = false;
    bool autoAddOverlay = false;
    bool doNotCompressAnything = false;
    std::unordered_set<std::string> extensionsToNotCompress;
    Maybe<std::string> privateSymbols;
    ManifestFixerOptions manifestFixerOptions;
    std::unordered_set<std::string> products;

    // Split APK options.
    TableSplitterOptions tableSplitterOptions;
    std::vector<SplitConstraints> splitConstraints;
    std::vector<std::string> splitPaths;

    // Stable ID options.
    std::unordered_map<ResourceName, ResourceId> stableIdMap;
    Maybe<std::string> resourceIdMapPath;
};

class LinkContext : public IAaptContext {
public:
    LinkContext() : mNameMangler({}) {
    }

    IDiagnostics* getDiagnostics() override {
        return &mDiagnostics;
    }

    NameMangler* getNameMangler() override {
        return &mNameMangler;
    }

    void setNameManglerPolicy(const NameManglerPolicy& policy) {
        mNameMangler = NameMangler(policy);
    }

    const std::string& getCompilationPackage() override {
        return mCompilationPackage;
    }

    void setCompilationPackage(const StringPiece& packageName) {
        mCompilationPackage = packageName.toString();
    }

    uint8_t getPackageId() override {
        return mPackageId;
    }

    void setPackageId(uint8_t id) {
        mPackageId = id;
    }

    SymbolTable* getExternalSymbols() override {
        return &mSymbols;
    }

    bool verbose() override {
        return mVerbose;
    }

    void setVerbose(bool val) {
        mVerbose = val;
    }

    int getMinSdkVersion() override {
        return mMinSdkVersion;
    }

    void setMinSdkVersion(int minSdk) {
        mMinSdkVersion = minSdk;
    }

private:
    StdErrDiagnostics mDiagnostics;
    NameMangler mNameMangler;
    std::string mCompilationPackage;
    uint8_t mPackageId = 0x0;
    SymbolTable mSymbols;
    bool mVerbose = false;
    int mMinSdkVersion = 0;
};

static bool copyFileToArchive(io::IFile* file, const std::string& outPath,
                              uint32_t compressionFlags,
                              IArchiveWriter* writer, IAaptContext* context) {
    std::unique_ptr<io::IData> data = file->openAsData();
    if (!data) {
        context->getDiagnostics()->error(DiagMessage(file->getSource())
                                         << "failed to open file");
        return false;
    }

    const uint8_t* buffer = reinterpret_cast<const uint8_t*>(data->data());
    const size_t bufferSize = data->size();

    if (context->verbose()) {
        context->getDiagnostics()->note(DiagMessage() << "writing " << outPath << " to archive");
    }

    if (writer->startEntry(outPath, compressionFlags)) {
        if (writer->writeEntry(buffer, bufferSize)) {
            if (writer->finishEntry()) {
                return true;
            }
        }
    }

    context->getDiagnostics()->error(DiagMessage() << "failed to write file " << outPath);
    return false;
}

static bool flattenXml(xml::XmlResource* xmlRes, const StringPiece& path, Maybe<size_t> maxSdkLevel,
                       bool keepRawValues, IArchiveWriter* writer, IAaptContext* context) {
    BigBuffer buffer(1024);
    XmlFlattenerOptions options = {};
    options.keepRawValues = keepRawValues;
    options.maxSdkLevel = maxSdkLevel;
    XmlFlattener flattener(&buffer, options);
    if (!flattener.consume(context, xmlRes)) {
        return false;
    }

    if (context->verbose()) {
        DiagMessage msg;
        msg << "writing " << path << " to archive";
        if (maxSdkLevel) {
            msg << " maxSdkLevel=" << maxSdkLevel.value() << " keepRawValues=" << keepRawValues;
        }
        context->getDiagnostics()->note(msg);
    }

    if (writer->startEntry(path, ArchiveEntry::kCompress)) {
        if (writer->writeEntry(buffer)) {
            if (writer->finishEntry()) {
                return true;
            }
        }
    }
    context->getDiagnostics()->error(DiagMessage() << "failed to write " << path << " to archive");
    return false;
}

static std::unique_ptr<ResourceTable> loadTableFromPb(const Source& source,
                                                      const void* data, size_t len,
                                                      IDiagnostics* diag) {
    pb::ResourceTable pbTable;
    if (!pbTable.ParseFromArray(data, len)) {
        diag->error(DiagMessage(source) << "invalid compiled table");
        return {};
    }

    std::unique_ptr<ResourceTable> table = deserializeTableFromPb(pbTable, source, diag);
    if (!table) {
        return {};
    }
    return table;
}

/**
 * Inflates an XML file from the source path.
 */
static std::unique_ptr<xml::XmlResource> loadXml(const std::string& path, IDiagnostics* diag) {
    std::ifstream fin(path, std::ifstream::binary);
    if (!fin) {
        diag->error(DiagMessage(path) << strerror(errno));
        return {};
    }
    return xml::inflate(&fin, diag, Source(path));
}

struct ResourceFileFlattenerOptions {
    bool noAutoVersion = false;
    bool noVersionVectors = false;
    bool noXmlNamespaces = false;
    bool keepRawValues = false;
    bool doNotCompressAnything = false;
    bool updateProguardSpec = false;
    std::unordered_set<std::string> extensionsToNotCompress;
};

class ResourceFileFlattener {
public:
    ResourceFileFlattener(const ResourceFileFlattenerOptions& options,
                          IAaptContext* context, proguard::KeepSet* keepSet) :
            mOptions(options), mContext(context), mKeepSet(keepSet) {
    }

    bool flatten(ResourceTable* table, IArchiveWriter* archiveWriter);

private:
    struct FileOperation {
        ConfigDescription config;

        // The entry this file came from.
        const ResourceEntry* entry;

        // The file to copy as-is.
        io::IFile* fileToCopy;

        // The XML to process and flatten.
        std::unique_ptr<xml::XmlResource> xmlToFlatten;

        // The destination to write this file to.
        std::string dstPath;
        bool skipVersion = false;
    };

    uint32_t getCompressionFlags(const StringPiece& str);

    bool linkAndVersionXmlFile(ResourceTable* table, FileOperation* fileOp,
                               std::queue<FileOperation>* outFileOpQueue);

    ResourceFileFlattenerOptions mOptions;
    IAaptContext* mContext;
    proguard::KeepSet* mKeepSet;
};

uint32_t ResourceFileFlattener::getCompressionFlags(const StringPiece& str) {
    if (mOptions.doNotCompressAnything) {
        return 0;
    }

    for (const std::string& extension : mOptions.extensionsToNotCompress) {
        if (util::stringEndsWith(str, extension)) {
            return 0;
        }
    }
    return ArchiveEntry::kCompress;
}

bool ResourceFileFlattener::linkAndVersionXmlFile(ResourceTable* table,
                                                  FileOperation* fileOp,
                                                  std::queue<FileOperation>* outFileOpQueue) {
    xml::XmlResource* doc = fileOp->xmlToFlatten.get();
    const Source& src = doc->file.source;

    if (mContext->verbose()) {
        mContext->getDiagnostics()->note(DiagMessage() << "linking " << src.path);
    }

    XmlReferenceLinker xmlLinker;
    if (!xmlLinker.consume(mContext, doc)) {
        return false;
    }

    if (mOptions.updateProguardSpec && !proguard::collectProguardRules(src, doc, mKeepSet)) {
        return false;
    }

    if (mOptions.noXmlNamespaces) {
        XmlNamespaceRemover namespaceRemover;
        if (!namespaceRemover.consume(mContext, doc)) {
            return false;
        }
    }

    if (!mOptions.noAutoVersion) {
        if (mOptions.noVersionVectors) {
            // Skip this if it is a vector or animated-vector.
            xml::Element* el = xml::findRootElement(doc);
            if (el && el->namespaceUri.empty()) {
                if (el->name == "vector" || el->name == "animated-vector") {
                    // We are NOT going to version this file.
                    fileOp->skipVersion = true;
                    return true;
                }
            }
        }

        const ConfigDescription& config = fileOp->config;

        // Find the first SDK level used that is higher than this defined config and
        // not superseded by a lower or equal SDK level resource.
        const int minSdkVersion = mContext->getMinSdkVersion();
        for (int sdkLevel : xmlLinker.getSdkLevels()) {
            if (sdkLevel > minSdkVersion && sdkLevel > config.sdkVersion) {
                if (!shouldGenerateVersionedResource(fileOp->entry, config, sdkLevel)) {
                    // If we shouldn't generate a versioned resource, stop checking.
                    break;
                }

                ResourceFile versionedFileDesc = doc->file;
                versionedFileDesc.config.sdkVersion = (uint16_t) sdkLevel;

                FileOperation newFileOp;
                newFileOp.xmlToFlatten = util::make_unique<xml::XmlResource>(
                        versionedFileDesc, doc->root->clone());
                newFileOp.config = versionedFileDesc.config;
                newFileOp.entry = fileOp->entry;
                newFileOp.dstPath = ResourceUtils::buildResourceFileName(
                        versionedFileDesc, mContext->getNameMangler());

                if (mContext->verbose()) {
                    mContext->getDiagnostics()->note(DiagMessage(versionedFileDesc.source)
                                                     << "auto-versioning resource from config '"
                                                     << config
                                                     << "' -> '"
                                                     << versionedFileDesc.config << "'");
                }

                bool added = table->addFileReferenceAllowMangled(versionedFileDesc.name,
                                                                 versionedFileDesc.config,
                                                                 versionedFileDesc.source,
                                                                 newFileOp.dstPath,
                                                                 nullptr,
                                                                 mContext->getDiagnostics());
                if (!added) {
                    return false;
                }

                outFileOpQueue->push(std::move(newFileOp));
                break;
            }
        }
    }
    return true;
}

/**
 * Do not insert or remove any resources while executing in this function. It will
 * corrupt the iteration order.
 */
bool ResourceFileFlattener::flatten(ResourceTable* table, IArchiveWriter* archiveWriter) {
    bool error = false;
    std::map<std::pair<ConfigDescription, StringPiece>, FileOperation> configSortedFiles;

    for (auto& pkg : table->packages) {
        for (auto& type : pkg->types) {
            // Sort by config and name, so that we get better locality in the zip file.
            configSortedFiles.clear();
            std::queue<FileOperation> fileOperations;

            // Populate the queue with all files in the ResourceTable.
            for (auto& entry : type->entries) {
                for (auto& configValue : entry->values) {
                    FileReference* fileRef = valueCast<FileReference>(configValue->value.get());
                    if (!fileRef) {
                        continue;
                    }

                    io::IFile* file = fileRef->file;
                    if (!file) {
                        mContext->getDiagnostics()->error(DiagMessage(fileRef->getSource())
                                                          << "file not found");
                        return false;
                    }

                    FileOperation fileOp;
                    fileOp.entry = entry.get();
                    fileOp.dstPath = *fileRef->path;
                    fileOp.config = configValue->config;

                    const StringPiece srcPath = file->getSource().path;
                    if (type->type != ResourceType::kRaw &&
                            (util::stringEndsWith(srcPath, ".xml.flat") ||
                            util::stringEndsWith(srcPath, ".xml"))) {
                        std::unique_ptr<io::IData> data = file->openAsData();
                        if (!data) {
                            mContext->getDiagnostics()->error(DiagMessage(file->getSource())
                                                              << "failed to open file");
                            return false;
                        }

                        fileOp.xmlToFlatten = xml::inflate(data->data(), data->size(),
                                                           mContext->getDiagnostics(),
                                                           file->getSource());

                        if (!fileOp.xmlToFlatten) {
                            return false;
                        }

                        fileOp.xmlToFlatten->file.config = configValue->config;
                        fileOp.xmlToFlatten->file.source = fileRef->getSource();
                        fileOp.xmlToFlatten->file.name =
                                ResourceName(pkg->name, type->type, entry->name);

                        // Enqueue the XML files to be processed.
                        fileOperations.push(std::move(fileOp));
                    } else {
                        fileOp.fileToCopy = file;

                        // NOTE(adamlesinski): Explicitly construct a StringPiece here, or else
                        // we end up copying the string in the std::make_pair() method, then
                        // creating a StringPiece from the copy, which would cause us to end up
                        // referencing garbage in the map.
                        const StringPiece entryName(entry->name);
                        configSortedFiles[std::make_pair(configValue->config, entryName)] =
                                std::move(fileOp);
                    }
                }
            }

            // Now process the XML queue
            for (; !fileOperations.empty(); fileOperations.pop()) {
                FileOperation& fileOp = fileOperations.front();

                if (!linkAndVersionXmlFile(table, &fileOp, &fileOperations)) {
                    error = true;
                    continue;
                }

                // NOTE(adamlesinski): Explicitly construct a StringPiece here, or else
                // we end up copying the string in the std::make_pair() method, then creating
                // a StringPiece from the copy, which would cause us to end up referencing
                // garbage in the map.
                const StringPiece entryName(fileOp.entry->name);
                configSortedFiles[std::make_pair(fileOp.config, entryName)] = std::move(fileOp);
            }

            if (error) {
                return false;
            }

            // Now flatten the sorted values.
            for (auto& mapEntry : configSortedFiles) {
                const ConfigDescription& config = mapEntry.first.first;
                const FileOperation& fileOp = mapEntry.second;

                if (fileOp.xmlToFlatten) {
                    Maybe<size_t> maxSdkLevel;
                    if (!mOptions.noAutoVersion && !fileOp.skipVersion) {
                        maxSdkLevel = std::max<size_t>(std::max<size_t>(config.sdkVersion, 1u),
                                                       mContext->getMinSdkVersion());
                    }

                    bool result = flattenXml(fileOp.xmlToFlatten.get(), fileOp.dstPath, maxSdkLevel,
                                             mOptions.keepRawValues,
                                             archiveWriter, mContext);
                    if (!result) {
                        error = true;
                    }
                } else {
                    bool result = copyFileToArchive(fileOp.fileToCopy, fileOp.dstPath,
                                                    getCompressionFlags(fileOp.dstPath),
                                                    archiveWriter, mContext);
                    if (!result) {
                        error = true;
                    }
                }
            }
        }
    }
    return !error;
}

static bool writeStableIdMapToPath(IDiagnostics* diag,
                                   const std::unordered_map<ResourceName, ResourceId>& idMap,
                                   const std::string& idMapPath) {
    std::ofstream fout(idMapPath, std::ofstream::binary);
    if (!fout) {
        diag->error(DiagMessage(idMapPath) << strerror(errno));
        return false;
    }

    for (const auto& entry : idMap) {
        const ResourceName& name = entry.first;
        const ResourceId& id = entry.second;
        fout << name << " = " << id << "\n";
    }

    if (!fout) {
        diag->error(DiagMessage(idMapPath) << "failed writing to file: " << strerror(errno));
        return false;
    }

    return true;
}

static bool loadStableIdMap(IDiagnostics* diag, const std::string& path,
                            std::unordered_map<ResourceName, ResourceId>* outIdMap) {
    std::string content;
    if (!android::base::ReadFileToString(path, &content)) {
        diag->error(DiagMessage(path) << "failed reading stable ID file");
        return false;
    }

    outIdMap->clear();
    size_t lineNo = 0;
    for (StringPiece line : util::tokenize(content, '\n')) {
        lineNo++;
        line = util::trimWhitespace(line);
        if (line.empty()) {
            continue;
        }

        auto iter = std::find(line.begin(), line.end(), '=');
        if (iter == line.end()) {
            diag->error(DiagMessage(Source(path, lineNo)) << "missing '='");
            return false;
        }

        ResourceNameRef name;
        StringPiece resNameStr = util::trimWhitespace(
                line.substr(0, std::distance(line.begin(), iter)));
        if (!ResourceUtils::parseResourceName(resNameStr, &name)) {
            diag->error(DiagMessage(Source(path, lineNo))
                        << "invalid resource name '" << resNameStr << "'");
            return false;
        }

        const size_t resIdStartIdx = std::distance(line.begin(), iter) + 1;
        const size_t resIdStrLen = line.size() - resIdStartIdx;
        StringPiece resIdStr = util::trimWhitespace(line.substr(resIdStartIdx, resIdStrLen));

        Maybe<ResourceId> maybeId = ResourceUtils::parseResourceId(resIdStr);
        if (!maybeId) {
            diag->error(DiagMessage(Source(path, lineNo)) << "invalid resource ID '"
                        << resIdStr << "'");
            return false;
        }

        (*outIdMap)[name.toResourceName()] = maybeId.value();
    }
    return true;
}

static bool parseSplitParameter(const StringPiece& arg, IDiagnostics* diag,
                                std::string* outPath, SplitConstraints* outSplit) {
    std::vector<std::string> parts = util::split(arg, ':');
    if (parts.size() != 2) {
        diag->error(DiagMessage() << "invalid split parameter '" << arg << "'");
        diag->note(DiagMessage() << "should be --split path/to/output.apk:<config>[,<config>...]");
        return false;
    }
    *outPath = parts[0];
    std::vector<ConfigDescription> configs;
    for (const StringPiece& configStr : util::tokenize(parts[1], ',')) {
        configs.push_back({});
        if (!ConfigDescription::parse(configStr, &configs.back())) {
            diag->error(DiagMessage() << "invalid config '" << configStr
                        << "' in split parameter '" << arg << "'");
            return false;
        }
    }
    outSplit->configs.insert(configs.begin(), configs.end());
    return true;
}

class LinkCommand {
public:
    LinkCommand(LinkContext* context, const LinkOptions& options) :
            mOptions(options), mContext(context), mFinalTable(),
            mFileCollection(util::make_unique<io::FileCollection>()) {
    }

    /**
     * Creates a SymbolTable that loads symbols from the various APKs and caches the
     * results for faster lookup.
     */
    bool loadSymbolsFromIncludePaths() {
        std::unique_ptr<AssetManagerSymbolSource> assetSource =
                util::make_unique<AssetManagerSymbolSource>();
        for (const std::string& path : mOptions.includePaths) {
            if (mContext->verbose()) {
                mContext->getDiagnostics()->note(DiagMessage(path) << "loading include path");
            }

            // First try to load the file as a static lib.
            std::string errorStr;
            std::unique_ptr<ResourceTable> staticInclude = loadStaticLibrary(path, &errorStr);
            if (staticInclude) {
                if (!mOptions.staticLib) {
                    // Can't include static libraries when not building a static library.
                    mContext->getDiagnostics()->error(
                            DiagMessage(path) << "can't include static library when building app");
                    return false;
                }

                // If we are using --no-static-lib-packages, we need to rename the package of this
                // table to our compilation package.
                if (mOptions.noStaticLibPackages) {
                    if (ResourceTablePackage* pkg = staticInclude->findPackageById(0x7f)) {
                        pkg->name = mContext->getCompilationPackage();
                    }
                }

                mContext->getExternalSymbols()->appendSource(
                        util::make_unique<ResourceTableSymbolSource>(staticInclude.get()));

                mStaticTableIncludes.push_back(std::move(staticInclude));

            } else if (!errorStr.empty()) {
                // We had an error with reading, so fail.
                mContext->getDiagnostics()->error(DiagMessage(path) << errorStr);
                return false;
            }

            if (!assetSource->addAssetPath(path)) {
                mContext->getDiagnostics()->error(
                        DiagMessage(path) << "failed to load include path");
                return false;
            }
        }

        mContext->getExternalSymbols()->appendSource(std::move(assetSource));
        return true;
    }

    Maybe<AppInfo> extractAppInfoFromManifest(xml::XmlResource* xmlRes, IDiagnostics* diag) {
        // Make sure the first element is <manifest> with package attribute.
        if (xml::Element* manifestEl = xml::findRootElement(xmlRes->root.get())) {
            AppInfo appInfo;

            if (!manifestEl->namespaceUri.empty() || manifestEl->name != "manifest") {
                diag->error(DiagMessage(xmlRes->file.source) << "root tag must be <manifest>");
                return {};
            }

            xml::Attribute* packageAttr = manifestEl->findAttribute({}, "package");
            if (!packageAttr) {
                diag->error(DiagMessage(xmlRes->file.source)
                            << "<manifest> must have a 'package' attribute");
                return {};
            }

            appInfo.package = packageAttr->value;

            if (xml::Attribute* versionCodeAttr =
                    manifestEl->findAttribute(xml::kSchemaAndroid, "versionCode")) {
                Maybe<uint32_t> maybeCode = ResourceUtils::parseInt(versionCodeAttr->value);
                if (!maybeCode) {
                    diag->error(DiagMessage(xmlRes->file.source.withLine(manifestEl->lineNumber))
                                << "invalid android:versionCode '"
                                << versionCodeAttr->value << "'");
                    return {};
                }
                appInfo.versionCode = maybeCode.value();
            }

            if (xml::Attribute* revisionCodeAttr =
                    manifestEl->findAttribute(xml::kSchemaAndroid, "revisionCode")) {
                Maybe<uint32_t> maybeCode = ResourceUtils::parseInt(revisionCodeAttr->value);
                if (!maybeCode) {
                    diag->error(DiagMessage(xmlRes->file.source.withLine(manifestEl->lineNumber))
                                << "invalid android:revisionCode '"
                                << revisionCodeAttr->value << "'");
                    return {};
                }
                appInfo.revisionCode = maybeCode.value();
            }

            if (xml::Element* usesSdkEl = manifestEl->findChild({}, "uses-sdk")) {
                if (xml::Attribute* minSdk =
                        usesSdkEl->findAttribute(xml::kSchemaAndroid, "minSdkVersion")) {
                    appInfo.minSdkVersion = minSdk->value;
                }
            }

            return appInfo;
        }
        return {};
    }

    /**
     * Precondition: ResourceTable doesn't have any IDs assigned yet, nor is it linked.
     * Postcondition: ResourceTable has only one package left. All others are stripped, or there
     *                is an error and false is returned.
     */
    bool verifyNoExternalPackages() {
        auto isExtPackageFunc = [&](const std::unique_ptr<ResourceTablePackage>& pkg) -> bool {
            return mContext->getCompilationPackage() != pkg->name ||
                    !pkg->id ||
                    pkg->id.value() != mContext->getPackageId();
        };

        bool error = false;
        for (const auto& package : mFinalTable.packages) {
            if (isExtPackageFunc(package)) {
                // We have a package that is not related to the one we're building!
                for (const auto& type : package->types) {
                    for (const auto& entry : type->entries) {
                        ResourceNameRef resName(package->name, type->type, entry->name);

                        for (const auto& configValue : entry->values) {
                            // Special case the occurrence of an ID that is being generated for the
                            // 'android' package. This is due to legacy reasons.
                            if (valueCast<Id>(configValue->value.get()) &&
                                    package->name == "android") {
                                mContext->getDiagnostics()->warn(
                                        DiagMessage(configValue->value->getSource())
                                        << "generated id '" << resName
                                        << "' for external package '" << package->name
                                        << "'");
                            } else {
                                mContext->getDiagnostics()->error(
                                        DiagMessage(configValue->value->getSource())
                                        << "defined resource '" << resName
                                        << "' for external package '" << package->name
                                        << "'");
                                error = true;
                            }
                        }
                    }
                }
            }
        }

        auto newEndIter = std::remove_if(mFinalTable.packages.begin(), mFinalTable.packages.end(),
                                         isExtPackageFunc);
        mFinalTable.packages.erase(newEndIter, mFinalTable.packages.end());
        return !error;
    }

    /**
     * Returns true if no IDs have been set, false otherwise.
     */
    bool verifyNoIdsSet() {
        for (const auto& package : mFinalTable.packages) {
            for (const auto& type : package->types) {
                if (type->id) {
                    mContext->getDiagnostics()->error(DiagMessage() << "type " << type->type
                                                      << " has ID " << std::hex
                                                      << (int) type->id.value()
                                                      << std::dec << " assigned");
                    return false;
                }

                for (const auto& entry : type->entries) {
                    if (entry->id) {
                        ResourceNameRef resName(package->name, type->type, entry->name);
                        mContext->getDiagnostics()->error(DiagMessage() << "entry " << resName
                                                          << " has ID " << std::hex
                                                          << (int) entry->id.value()
                                                          << std::dec << " assigned");
                        return false;
                    }
                }
            }
        }
        return true;
    }

    std::unique_ptr<IArchiveWriter> makeArchiveWriter(const StringPiece& out) {
        if (mOptions.outputToDirectory) {
            return createDirectoryArchiveWriter(mContext->getDiagnostics(), out);
        } else {
            return createZipFileArchiveWriter(mContext->getDiagnostics(), out);
        }
    }

    bool flattenTable(ResourceTable* table, IArchiveWriter* writer) {
        BigBuffer buffer(1024);
        TableFlattener flattener(&buffer);
        if (!flattener.consume(mContext, table)) {
            return false;
        }

        if (writer->startEntry("resources.arsc", ArchiveEntry::kAlign)) {
            if (writer->writeEntry(buffer)) {
                if (writer->finishEntry()) {
                    return true;
                }
            }
        }

        mContext->getDiagnostics()->error(
                DiagMessage() << "failed to write resources.arsc to archive");
        return false;
    }

    bool flattenTableToPb(ResourceTable* table, IArchiveWriter* writer) {
        // Create the file/zip entry.
        if (!writer->startEntry("resources.arsc.flat", 0)) {
            mContext->getDiagnostics()->error(DiagMessage() << "failed to open");
            return false;
        }

        // Make sure CopyingOutputStreamAdaptor is deleted before we call writer->finishEntry().
        {
            // Wrap our IArchiveWriter with an adaptor that implements the ZeroCopyOutputStream
            // interface.
            CopyingOutputStreamAdaptor adaptor(writer);

            std::unique_ptr<pb::ResourceTable> pbTable = serializeTableToPb(table);
            if (!pbTable->SerializeToZeroCopyStream(&adaptor)) {
                mContext->getDiagnostics()->error(DiagMessage() << "failed to write");
                return false;
            }
        }

        if (!writer->finishEntry()) {
            mContext->getDiagnostics()->error(DiagMessage() << "failed to finish entry");
            return false;
        }
        return true;
    }

    bool writeJavaFile(ResourceTable* table, const StringPiece& packageNameToGenerate,
                       const StringPiece& outPackage, const JavaClassGeneratorOptions& javaOptions) {
        if (!mOptions.generateJavaClassPath) {
            return true;
        }

        std::string outPath = mOptions.generateJavaClassPath.value();
        file::appendPath(&outPath, file::packageToPath(outPackage));
        if (!file::mkdirs(outPath)) {
            mContext->getDiagnostics()->error(
                    DiagMessage() << "failed to create directory '" << outPath << "'");
            return false;
        }

        file::appendPath(&outPath, "R.java");

        std::ofstream fout(outPath, std::ofstream::binary);
        if (!fout) {
            mContext->getDiagnostics()->error(
                    DiagMessage() << "failed writing to '" << outPath << "': " << strerror(errno));
            return false;
        }

        JavaClassGenerator generator(mContext, table, javaOptions);
        if (!generator.generate(packageNameToGenerate, outPackage, &fout)) {
            mContext->getDiagnostics()->error(DiagMessage(outPath) << generator.getError());
            return false;
        }

        if (!fout) {
            mContext->getDiagnostics()->error(
                    DiagMessage() << "failed writing to '" << outPath << "': " << strerror(errno));
        }
        return true;
    }

    bool writeManifestJavaFile(xml::XmlResource* manifestXml) {
        if (!mOptions.generateJavaClassPath) {
            return true;
        }

        std::unique_ptr<ClassDefinition> manifestClass = generateManifestClass(
                mContext->getDiagnostics(), manifestXml);

        if (!manifestClass) {
            // Something bad happened, but we already logged it, so exit.
            return false;
        }

        if (manifestClass->empty()) {
            // Empty Manifest class, no need to generate it.
            return true;
        }

        // Add any JavaDoc annotations to the generated class.
        for (const std::string& annotation : mOptions.javadocAnnotations) {
            std::string properAnnotation = "@";
            properAnnotation += annotation;
            manifestClass->getCommentBuilder()->appendComment(properAnnotation);
        }

        const std::string& packageUtf8 = mContext->getCompilationPackage();

        std::string outPath = mOptions.generateJavaClassPath.value();
        file::appendPath(&outPath, file::packageToPath(packageUtf8));

        if (!file::mkdirs(outPath)) {
            mContext->getDiagnostics()->error(
                    DiagMessage() << "failed to create directory '" << outPath << "'");
            return false;
        }

        file::appendPath(&outPath, "Manifest.java");

        std::ofstream fout(outPath, std::ofstream::binary);
        if (!fout) {
            mContext->getDiagnostics()->error(
                    DiagMessage() << "failed writing to '" << outPath << "': " << strerror(errno));
            return false;
        }

        if (!ClassDefinition::writeJavaFile(manifestClass.get(), packageUtf8, true, &fout)) {
            mContext->getDiagnostics()->error(
                    DiagMessage() << "failed writing to '" << outPath << "': " << strerror(errno));
            return false;
        }
        return true;
    }

    bool writeProguardFile(const Maybe<std::string>& out, const proguard::KeepSet& keepSet) {
        if (!out) {
            return true;
        }

        const std::string& outPath = out.value();
        std::ofstream fout(outPath, std::ofstream::binary);
        if (!fout) {
            mContext->getDiagnostics()->error(
                    DiagMessage() << "failed to open '" << outPath << "': " << strerror(errno));
            return false;
        }

        proguard::writeKeepSet(&fout, keepSet);
        if (!fout) {
            mContext->getDiagnostics()->error(
                    DiagMessage() << "failed writing to '" << outPath << "': " << strerror(errno));
            return false;
        }
        return true;
    }

    std::unique_ptr<ResourceTable> loadStaticLibrary(const std::string& input,
                                                     std::string* outError) {
        std::unique_ptr<io::ZipFileCollection> collection = io::ZipFileCollection::create(
                input, outError);
        if (!collection) {
            return {};
        }
        return loadTablePbFromCollection(collection.get());
    }

    std::unique_ptr<ResourceTable> loadTablePbFromCollection(io::IFileCollection* collection) {
        io::IFile* file = collection->findFile("resources.arsc.flat");
        if (!file) {
            return {};
        }

        std::unique_ptr<io::IData> data = file->openAsData();
        return loadTableFromPb(file->getSource(), data->data(), data->size(),
                               mContext->getDiagnostics());
    }

    bool mergeStaticLibrary(const std::string& input, bool override) {
        if (mContext->verbose()) {
            mContext->getDiagnostics()->note(DiagMessage() << "merging static library " << input);
        }

        std::string errorStr;
        std::unique_ptr<io::ZipFileCollection> collection =
                io::ZipFileCollection::create(input, &errorStr);
        if (!collection) {
            mContext->getDiagnostics()->error(DiagMessage(input) << errorStr);
            return false;
        }

        std::unique_ptr<ResourceTable> table = loadTablePbFromCollection(collection.get());
        if (!table) {
            mContext->getDiagnostics()->error(DiagMessage(input) << "invalid static library");
            return false;
        }

        ResourceTablePackage* pkg = table->findPackageById(0x7f);
        if (!pkg) {
            mContext->getDiagnostics()->error(DiagMessage(input)
                                              << "static library has no package");
            return false;
        }

        bool result;
        if (mOptions.noStaticLibPackages) {
            // Merge all resources as if they were in the compilation package. This is the old
            // behaviour of aapt.

            // Add the package to the set of --extra-packages so we emit an R.java for each
            // library package.
            if (!pkg->name.empty()) {
                mOptions.extraJavaPackages.insert(pkg->name);
            }

            pkg->name = "";
            if (override) {
                result = mTableMerger->mergeOverlay(Source(input), table.get(), collection.get());
            } else {
                result = mTableMerger->merge(Source(input), table.get(), collection.get());
            }

        } else {
            // This is the proper way to merge libraries, where the package name is preserved
            // and resource names are mangled.
            result = mTableMerger->mergeAndMangle(Source(input), pkg->name, table.get(),
                                                  collection.get());
        }

        if (!result) {
            return false;
        }

        // Make sure to move the collection into the set of IFileCollections.
        mCollections.push_back(std::move(collection));
        return true;
    }

    bool mergeResourceTable(io::IFile* file, bool override) {
        if (mContext->verbose()) {
            mContext->getDiagnostics()->note(DiagMessage() << "merging resource table "
                                             << file->getSource());
        }

        std::unique_ptr<io::IData> data = file->openAsData();
        if (!data) {
            mContext->getDiagnostics()->error(DiagMessage(file->getSource())
                                             << "failed to open file");
            return false;
        }

        std::unique_ptr<ResourceTable> table = loadTableFromPb(file->getSource(),
                                                               data->data(), data->size(),
                                                               mContext->getDiagnostics());
        if (!table) {
            return false;
        }

        bool result = false;
        if (override) {
            result = mTableMerger->mergeOverlay(file->getSource(), table.get());
        } else {
            result = mTableMerger->merge(file->getSource(), table.get());
        }
        return result;
    }

    bool mergeCompiledFile(io::IFile* file, ResourceFile* fileDesc, bool override) {
        if (mContext->verbose()) {
            mContext->getDiagnostics()->note(DiagMessage()
                                             << "merging '" << fileDesc->name
                                             << "' from compiled file "
                                             << file->getSource());
        }

        bool result = false;
        if (override) {
            result = mTableMerger->mergeFileOverlay(*fileDesc, file);
        } else {
            result = mTableMerger->mergeFile(*fileDesc, file);
        }

        if (!result) {
            return false;
        }

        // Add the exports of this file to the table.
        for (SourcedResourceName& exportedSymbol : fileDesc->exportedSymbols) {
            if (exportedSymbol.name.package.empty()) {
                exportedSymbol.name.package = mContext->getCompilationPackage();
            }

            ResourceNameRef resName = exportedSymbol.name;

            Maybe<ResourceName> mangledName = mContext->getNameMangler()->mangleName(
                    exportedSymbol.name);
            if (mangledName) {
                resName = mangledName.value();
            }

            std::unique_ptr<Id> id = util::make_unique<Id>();
            id->setSource(fileDesc->source.withLine(exportedSymbol.line));
            bool result = mFinalTable.addResourceAllowMangled(
                    resName, ConfigDescription::defaultConfig(), std::string(), std::move(id),
                    mContext->getDiagnostics());
            if (!result) {
                return false;
            }
        }
        return true;
    }

    /**
     * Takes a path to load as a ZIP file and merges the files within into the master ResourceTable.
     * If override is true, conflicting resources are allowed to override each other, in order of
     * last seen.
     *
     * An io::IFileCollection is created from the ZIP file and added to the set of
     * io::IFileCollections that are open.
     */
    bool mergeArchive(const std::string& input, bool override) {
        if (mContext->verbose()) {
            mContext->getDiagnostics()->note(DiagMessage() << "merging archive " << input);
        }

        std::string errorStr;
        std::unique_ptr<io::ZipFileCollection> collection =
                io::ZipFileCollection::create(input, &errorStr);
        if (!collection) {
            mContext->getDiagnostics()->error(DiagMessage(input) << errorStr);
            return false;
        }

        bool error = false;
        for (auto iter = collection->iterator(); iter->hasNext(); ) {
            if (!mergeFile(iter->next(), override)) {
                error = true;
            }
        }

        // Make sure to move the collection into the set of IFileCollections.
        mCollections.push_back(std::move(collection));
        return !error;
    }

    /**
     * Takes a path to load and merge into the master ResourceTable. If override is true,
     * conflicting resources are allowed to override each other, in order of last seen.
     *
     * If the file path ends with .flata, .jar, .jack, or .zip the file is treated as ZIP archive
     * and the files within are merged individually.
     *
     * Otherwise the files is processed on its own.
     */
    bool mergePath(const std::string& path, bool override) {
        if (util::stringEndsWith(path, ".flata") ||
                util::stringEndsWith(path, ".jar") ||
                util::stringEndsWith(path, ".jack") ||
                util::stringEndsWith(path, ".zip")) {
            return mergeArchive(path, override);
        } else if (util::stringEndsWith(path, ".apk")) {
            return mergeStaticLibrary(path, override);
        }

        io::IFile* file = mFileCollection->insertFile(path);
        return mergeFile(file, override);
    }

    /**
     * Takes a file to load and merge into the master ResourceTable. If override is true,
     * conflicting resources are allowed to override each other, in order of last seen.
     *
     * If the file ends with .arsc.flat, then it is loaded as a ResourceTable and merged into the
     * master ResourceTable. If the file ends with .flat, then it is treated like a compiled file
     * and the header data is read and merged into the final ResourceTable.
     *
     * All other file types are ignored. This is because these files could be coming from a zip,
     * where we could have other files like classes.dex.
     */
    bool mergeFile(io::IFile* file, bool override) {
        const Source& src = file->getSource();
        if (util::stringEndsWith(src.path, ".arsc.flat")) {
            return mergeResourceTable(file, override);

        } else if (util::stringEndsWith(src.path, ".flat")){
            // Try opening the file and looking for an Export header.
            std::unique_ptr<io::IData> data = file->openAsData();
            if (!data) {
                mContext->getDiagnostics()->error(DiagMessage(src) << "failed to open");
                return false;
            }

            CompiledFileInputStream inputStream(data->data(), data->size());
            uint32_t numFiles = 0;
            if (!inputStream.ReadLittleEndian32(&numFiles)) {
                mContext->getDiagnostics()->error(DiagMessage(src) << "failed read num files");
                return false;
            }

            for (uint32_t i = 0; i < numFiles; i++) {
                pb::CompiledFile compiledFile;
                if (!inputStream.ReadCompiledFile(&compiledFile)) {
                    mContext->getDiagnostics()->error(DiagMessage(src)
                                                      << "failed to read compiled file header");
                    return false;
                }

                uint64_t offset, len;
                if (!inputStream.ReadDataMetaData(&offset, &len)) {
                    mContext->getDiagnostics()->error(DiagMessage(src)
                                                      << "failed to read data meta data");
                    return false;
                }

                std::unique_ptr<ResourceFile> resourceFile = deserializeCompiledFileFromPb(
                        compiledFile, file->getSource(), mContext->getDiagnostics());
                if (!resourceFile) {
                    return false;
                }

                if (!mergeCompiledFile(file->createFileSegment(offset, len), resourceFile.get(),
                                       override)) {
                    return false;
                }
            }
            return true;
        }

        // Ignore non .flat files. This could be classes.dex or something else that happens
        // to be in an archive.
        return true;
    }

    std::unique_ptr<xml::XmlResource> generateSplitManifest(const AppInfo& appInfo,
                                                            const SplitConstraints& constraints) {
        std::unique_ptr<xml::XmlResource> doc = util::make_unique<xml::XmlResource>();

        std::unique_ptr<xml::Namespace> namespaceAndroid = util::make_unique<xml::Namespace>();
        namespaceAndroid->namespaceUri = xml::kSchemaAndroid;
        namespaceAndroid->namespacePrefix = "android";

        std::unique_ptr<xml::Element> manifestEl = util::make_unique<xml::Element>();
        manifestEl->name = "manifest";
        manifestEl->attributes.push_back(
                xml::Attribute{ "", "package", appInfo.package });

        if (appInfo.versionCode) {
            manifestEl->attributes.push_back(xml::Attribute{
                    xml::kSchemaAndroid,
                    "versionCode",
                    std::to_string(appInfo.versionCode.value()) });
        }

        if (appInfo.revisionCode) {
            manifestEl->attributes.push_back(xml::Attribute{
                    xml::kSchemaAndroid,
                    "revisionCode", std::to_string(appInfo.revisionCode.value()) });
        }

        std::stringstream splitName;
        splitName << "config." << util::joiner(constraints.configs, "_");

        manifestEl->attributes.push_back(
                xml::Attribute{ "", "split", splitName.str() });

        std::unique_ptr<xml::Element> applicationEl = util::make_unique<xml::Element>();
        applicationEl->name = "application";
        applicationEl->attributes.push_back(
                xml::Attribute{ xml::kSchemaAndroid, "hasCode", "false" });

        manifestEl->addChild(std::move(applicationEl));
        namespaceAndroid->addChild(std::move(manifestEl));
        doc->root = std::move(namespaceAndroid);
        return doc;
    }

    /**
     * Writes the AndroidManifest, ResourceTable, and all XML files referenced by the ResourceTable
     * to the IArchiveWriter.
     */
    bool writeApk(IArchiveWriter* writer, proguard::KeepSet* keepSet, xml::XmlResource* manifest,
                  ResourceTable* table) {
        const bool keepRawValues = mOptions.staticLib;
        bool result = flattenXml(manifest, "AndroidManifest.xml", {}, keepRawValues, writer,
                                 mContext);
        if (!result) {
            return false;
        }

        ResourceFileFlattenerOptions fileFlattenerOptions;
        fileFlattenerOptions.keepRawValues = keepRawValues;
        fileFlattenerOptions.doNotCompressAnything = mOptions.doNotCompressAnything;
        fileFlattenerOptions.extensionsToNotCompress = mOptions.extensionsToNotCompress;
        fileFlattenerOptions.noAutoVersion = mOptions.noAutoVersion;
        fileFlattenerOptions.noVersionVectors = mOptions.noVersionVectors;
        fileFlattenerOptions.noXmlNamespaces = mOptions.noXmlNamespaces;
        fileFlattenerOptions.updateProguardSpec =
                static_cast<bool>(mOptions.generateProguardRulesPath);

        ResourceFileFlattener fileFlattener(fileFlattenerOptions, mContext, keepSet);

        if (!fileFlattener.flatten(table, writer)) {
            mContext->getDiagnostics()->error(DiagMessage() << "failed linking file resources");
            return false;
        }

        if (mOptions.staticLib) {
            if (!flattenTableToPb(table, writer)) {
                mContext->getDiagnostics()->error(DiagMessage()
                                                  << "failed to write resources.arsc.flat");
                return false;
            }
        } else {
            if (!flattenTable(table, writer)) {
                mContext->getDiagnostics()->error(DiagMessage()
                                                  << "failed to write resources.arsc");
                return false;
            }
        }
        return true;
    }

    int run(const std::vector<std::string>& inputFiles) {
        // Load the AndroidManifest.xml
        std::unique_ptr<xml::XmlResource> manifestXml = loadXml(mOptions.manifestPath,
                                                                mContext->getDiagnostics());
        if (!manifestXml) {
            return 1;
        }

        // First extract the Package name without modifying it (via --rename-manifest-package).
        if (Maybe<AppInfo> maybeAppInfo = extractAppInfoFromManifest(manifestXml.get(),
                                                                     mContext->getDiagnostics())) {
            const AppInfo& appInfo = maybeAppInfo.value();
            mContext->setCompilationPackage(appInfo.package);
        }

        ManifestFixer manifestFixer(mOptions.manifestFixerOptions);
        if (!manifestFixer.consume(mContext, manifestXml.get())) {
            return 1;
        }

        Maybe<AppInfo> maybeAppInfo = extractAppInfoFromManifest(manifestXml.get(),
                                                                 mContext->getDiagnostics());
        if (!maybeAppInfo) {
            return 1;
        }

        const AppInfo& appInfo = maybeAppInfo.value();
        if (appInfo.minSdkVersion) {
            if (Maybe<int> maybeMinSdkVersion =
                    ResourceUtils::parseSdkVersion(appInfo.minSdkVersion.value())) {
                mContext->setMinSdkVersion(maybeMinSdkVersion.value());
            }
        }

        mContext->setNameManglerPolicy(NameManglerPolicy{ mContext->getCompilationPackage() });
        if (mContext->getCompilationPackage() == "android") {
            mContext->setPackageId(0x01);
        } else {
            mContext->setPackageId(0x7f);
        }

        if (!loadSymbolsFromIncludePaths()) {
            return 1;
        }

        TableMergerOptions tableMergerOptions;
        tableMergerOptions.autoAddOverlay = mOptions.autoAddOverlay;
        mTableMerger = util::make_unique<TableMerger>(mContext, &mFinalTable, tableMergerOptions);

        if (mContext->verbose()) {
            mContext->getDiagnostics()->note(
                    DiagMessage() << "linking package '" << mContext->getCompilationPackage()
                                  << "' with package ID " << std::hex
                                  << (int) mContext->getPackageId());
        }


        for (const std::string& input : inputFiles) {
            if (!mergePath(input, false)) {
                mContext->getDiagnostics()->error(DiagMessage() << "failed parsing input");
                return 1;
            }
        }

        for (const std::string& input : mOptions.overlayFiles) {
            if (!mergePath(input, true)) {
                mContext->getDiagnostics()->error(DiagMessage() << "failed parsing overlays");
                return 1;
            }
        }

        if (!verifyNoExternalPackages()) {
            return 1;
        }

        if (!mOptions.staticLib) {
            PrivateAttributeMover mover;
            if (!mover.consume(mContext, &mFinalTable)) {
                mContext->getDiagnostics()->error(
                        DiagMessage() << "failed moving private attributes");
                return 1;
            }

            // Assign IDs if we are building a regular app.
            IdAssigner idAssigner(&mOptions.stableIdMap);
            if (!idAssigner.consume(mContext, &mFinalTable)) {
                mContext->getDiagnostics()->error(DiagMessage() << "failed assigning IDs");
                return 1;
            }

            // Now grab each ID and emit it as a file.
            if (mOptions.resourceIdMapPath) {
                for (auto& package : mFinalTable.packages) {
                    for (auto& type : package->types) {
                        for (auto& entry : type->entries) {
                            ResourceName name(package->name, type->type, entry->name);
                            // The IDs are guaranteed to exist.
                            mOptions.stableIdMap[std::move(name)] = ResourceId(package->id.value(),
                                                                               type->id.value(),
                                                                               entry->id.value());
                        }
                    }
                }

                if (!writeStableIdMapToPath(mContext->getDiagnostics(),
                                            mOptions.stableIdMap,
                                            mOptions.resourceIdMapPath.value())) {
                    return 1;
                }
            }
        } else {
            // Static libs are merged with other apps, and ID collisions are bad, so verify that
            // no IDs have been set.
            if (!verifyNoIdsSet()) {
                return 1;
            }
        }

        // Add the names to mangle based on our source merge earlier.
        mContext->setNameManglerPolicy(NameManglerPolicy{
                mContext->getCompilationPackage(), mTableMerger->getMergedPackages() });

        // Add our table to the symbol table.
        mContext->getExternalSymbols()->prependSource(
                        util::make_unique<ResourceTableSymbolSource>(&mFinalTable));

        ReferenceLinker linker;
        if (!linker.consume(mContext, &mFinalTable)) {
            mContext->getDiagnostics()->error(DiagMessage() << "failed linking references");
            return 1;
        }

        if (mOptions.staticLib) {
            if (!mOptions.products.empty()) {
                mContext->getDiagnostics()->warn(
                        DiagMessage() << "can't select products when building static library");
            }
        } else {
            ProductFilter productFilter(mOptions.products);
            if (!productFilter.consume(mContext, &mFinalTable)) {
                mContext->getDiagnostics()->error(DiagMessage() << "failed stripping products");
                return 1;
            }
        }

        if (!mOptions.noAutoVersion) {
            AutoVersioner versioner;
            if (!versioner.consume(mContext, &mFinalTable)) {
                mContext->getDiagnostics()->error(DiagMessage() << "failed versioning styles");
                return 1;
            }
        }

        if (!mOptions.staticLib && mContext->getMinSdkVersion() > 0) {
            if (mContext->verbose()) {
                mContext->getDiagnostics()->note(
                        DiagMessage() << "collapsing resource versions for minimum SDK "
                        << mContext->getMinSdkVersion());
            }

            VersionCollapser collapser;
            if (!collapser.consume(mContext, &mFinalTable)) {
                return 1;
            }
        }

        proguard::KeepSet proguardKeepSet;
        proguard::KeepSet proguardMainDexKeepSet;

        if (mOptions.staticLib) {
            if (mOptions.tableSplitterOptions.configFilter != nullptr ||
                    mOptions.tableSplitterOptions.preferredDensity) {
                mContext->getDiagnostics()->warn(
                        DiagMessage() << "can't strip resources when building static library");
            }
        } else {
            // Adjust the SplitConstraints so that their SDK version is stripped if it is less
            // than or equal to the minSdk. Otherwise the resources that have had their SDK version
            // stripped due to minSdk won't ever match.
            std::vector<SplitConstraints> adjustedConstraintsList;
            adjustedConstraintsList.reserve(mOptions.splitConstraints.size());
            for (const SplitConstraints& constraints : mOptions.splitConstraints) {
                SplitConstraints adjustedConstraints;
                for (const ConfigDescription& config : constraints.configs) {
                    if (config.sdkVersion <= mContext->getMinSdkVersion()) {
                        adjustedConstraints.configs.insert(config.copyWithoutSdkVersion());
                    } else {
                        adjustedConstraints.configs.insert(config);
                    }
                }
                adjustedConstraintsList.push_back(std::move(adjustedConstraints));
            }

            TableSplitter tableSplitter(adjustedConstraintsList, mOptions.tableSplitterOptions);
            if (!tableSplitter.verifySplitConstraints(mContext)) {
                return 1;
            }
            tableSplitter.splitTable(&mFinalTable);

            // Now we need to write out the Split APKs.
            auto pathIter = mOptions.splitPaths.begin();
            auto splitConstraintsIter = adjustedConstraintsList.begin();
            for (std::unique_ptr<ResourceTable>& splitTable : tableSplitter.getSplits()) {
                if (mContext->verbose()) {
                    mContext->getDiagnostics()->note(
                            DiagMessage(*pathIter) << "generating split with configurations '"
                            << util::joiner(splitConstraintsIter->configs, ", ") << "'");
                }

                std::unique_ptr<IArchiveWriter> archiveWriter = makeArchiveWriter(*pathIter);
                if (!archiveWriter) {
                    mContext->getDiagnostics()->error(DiagMessage() << "failed to create archive");
                    return 1;
                }

                // Generate an AndroidManifest.xml for each split.
                std::unique_ptr<xml::XmlResource> splitManifest =
                        generateSplitManifest(appInfo, *splitConstraintsIter);

                XmlReferenceLinker linker;
                if (!linker.consume(mContext, splitManifest.get())) {
                    mContext->getDiagnostics()->error(
                            DiagMessage() << "failed to create Split AndroidManifest.xml");
                    return 1;
                }

                if (!writeApk(archiveWriter.get(), &proguardKeepSet, splitManifest.get(),
                              splitTable.get())) {
                    return 1;
                }

                ++pathIter;
                ++splitConstraintsIter;
            }
        }

        // Start writing the base APK.
        std::unique_ptr<IArchiveWriter> archiveWriter = makeArchiveWriter(mOptions.outputPath);
        if (!archiveWriter) {
            mContext->getDiagnostics()->error(DiagMessage() << "failed to create archive");
            return 1;
        }

        bool error = false;
        {
            // AndroidManifest.xml has no resource name, but the CallSite is built from the name
            // (aka, which package the AndroidManifest.xml is coming from).
            // So we give it a package name so it can see local resources.
            manifestXml->file.name.package = mContext->getCompilationPackage();

            XmlReferenceLinker manifestLinker;
            if (manifestLinker.consume(mContext, manifestXml.get())) {
                if (mOptions.generateProguardRulesPath &&
                        !proguard::collectProguardRulesForManifest(Source(mOptions.manifestPath),
                                                                   manifestXml.get(),
                                                                   &proguardKeepSet)) {
                    error = true;
                }

                if (mOptions.generateMainDexProguardRulesPath &&
                        !proguard::collectProguardRulesForManifest(Source(mOptions.manifestPath),
                                                                   manifestXml.get(),
                                                                   &proguardMainDexKeepSet,
                                                                   true)) {
                    error = true;
                }

                if (mOptions.generateJavaClassPath) {
                    if (!writeManifestJavaFile(manifestXml.get())) {
                        error = true;
                    }
                }

                if (mOptions.noXmlNamespaces) {
                    // PackageParser will fail if URIs are removed from AndroidManifest.xml.
                    XmlNamespaceRemover namespaceRemover(true /* keepUris */);
                    if (!namespaceRemover.consume(mContext, manifestXml.get())) {
                        error = true;
                    }
                }
            } else {
                error = true;
            }
        }

        if (error) {
            mContext->getDiagnostics()->error(DiagMessage() << "failed processing manifest");
            return 1;
        }

        if (!writeApk(archiveWriter.get(), &proguardKeepSet, manifestXml.get(), &mFinalTable)) {
            return 1;
        }

        if (mOptions.generateJavaClassPath) {
            JavaClassGeneratorOptions options;
            options.types = JavaClassGeneratorOptions::SymbolTypes::kAll;
            options.javadocAnnotations = mOptions.javadocAnnotations;

            if (mOptions.staticLib || mOptions.generateNonFinalIds) {
                options.useFinal = false;
            }

            const StringPiece actualPackage = mContext->getCompilationPackage();
            StringPiece outputPackage = mContext->getCompilationPackage();
            if (mOptions.customJavaPackage) {
                // Override the output java package to the custom one.
                outputPackage = mOptions.customJavaPackage.value();
            }

            if (mOptions.privateSymbols) {
                // If we defined a private symbols package, we only emit Public symbols
                // to the original package, and private and public symbols to the private package.

                options.types = JavaClassGeneratorOptions::SymbolTypes::kPublic;
                if (!writeJavaFile(&mFinalTable, mContext->getCompilationPackage(),
                                   outputPackage, options)) {
                    return 1;
                }

                options.types = JavaClassGeneratorOptions::SymbolTypes::kPublicPrivate;
                outputPackage = mOptions.privateSymbols.value();
            }

            if (!writeJavaFile(&mFinalTable, actualPackage, outputPackage, options)) {
                return 1;
            }

            for (const std::string& extraPackage : mOptions.extraJavaPackages) {
                if (!writeJavaFile(&mFinalTable, actualPackage, extraPackage, options)) {
                    return 1;
                }
            }
        }

        if (!writeProguardFile(mOptions.generateProguardRulesPath, proguardKeepSet)) {
            return 1;
        }

        if (!writeProguardFile(mOptions.generateMainDexProguardRulesPath, proguardMainDexKeepSet)) {
            return 1;
        }

        if (mContext->verbose()) {
            DebugPrintTableOptions debugPrintTableOptions;
            debugPrintTableOptions.showSources = true;
            Debug::printTable(&mFinalTable, debugPrintTableOptions);
        }
        return 0;
    }

private:
    LinkOptions mOptions;
    LinkContext* mContext;
    ResourceTable mFinalTable;

    std::unique_ptr<TableMerger> mTableMerger;

    // A pointer to the FileCollection representing the filesystem (not archives).
    std::unique_ptr<io::FileCollection> mFileCollection;

    // A vector of IFileCollections. This is mainly here to keep ownership of the collections.
    std::vector<std::unique_ptr<io::IFileCollection>> mCollections;

    // A vector of ResourceTables. This is here to retain ownership, so that the SymbolTable
    // can use these.
    std::vector<std::unique_ptr<ResourceTable>> mStaticTableIncludes;
};

int link(const std::vector<StringPiece>& args) {
    LinkContext context;
    LinkOptions options;
    std::vector<std::string> overlayArgList;
    std::vector<std::string> extraJavaPackages;
    Maybe<std::string> configs;
    Maybe<std::string> preferredDensity;
    Maybe<std::string> productList;
    bool legacyXFlag = false;
    bool requireLocalization = false;
    bool verbose = false;
    Maybe<std::string> stableIdFilePath;
    std::vector<std::string> splitArgs;
    Flags flags = Flags()
            .requiredFlag("-o", "Output path", &options.outputPath)
            .requiredFlag("--manifest", "Path to the Android manifest to build",
                          &options.manifestPath)
            .optionalFlagList("-I", "Adds an Android APK to link against", &options.includePaths)
            .optionalFlagList("-R", "Compilation unit to link, using `overlay` semantics.\n"
                              "The last conflicting resource given takes precedence.",
                              &overlayArgList)
            .optionalFlag("--java", "Directory in which to generate R.java",
                          &options.generateJavaClassPath)
            .optionalFlag("--proguard", "Output file for generated Proguard rules",
                          &options.generateProguardRulesPath)
            .optionalFlag("--proguard-main-dex",
                          "Output file for generated Proguard rules for the main dex",
                          &options.generateMainDexProguardRulesPath)
            .optionalSwitch("--no-auto-version",
                            "Disables automatic style and layout SDK versioning",
                            &options.noAutoVersion)
            .optionalSwitch("--no-version-vectors",
                            "Disables automatic versioning of vector drawables. Use this only\n"
                            "when building with vector drawable support library",
                            &options.noVersionVectors)
            .optionalSwitch("-x", "Legacy flag that specifies to use the package identifier 0x01",
                            &legacyXFlag)
            .optionalSwitch("-z", "Require localization of strings marked 'suggested'",
                            &requireLocalization)
            .optionalFlag("-c", "Comma separated list of configurations to include. The default\n"
                                "is all configurations", &configs)
            .optionalFlag("--preferred-density",
                          "Selects the closest matching density and strips out all others.",
                          &preferredDensity)
            .optionalFlag("--product", "Comma separated list of product names to keep",
                          &productList)
            .optionalSwitch("--output-to-dir", "Outputs the APK contents to a directory specified "
                            "by -o",
                            &options.outputToDirectory)
            .optionalSwitch("--no-xml-namespaces", "Removes XML namespace prefix and URI "
                            "information from AndroidManifest.xml\nand XML binaries in res/*.",
                            &options.noXmlNamespaces)
            .optionalFlag("--min-sdk-version", "Default minimum SDK version to use for "
                          "AndroidManifest.xml",
                          &options.manifestFixerOptions.minSdkVersionDefault)
            .optionalFlag("--target-sdk-version", "Default target SDK version to use for "
                          "AndroidManifest.xml",
                          &options.manifestFixerOptions.targetSdkVersionDefault)
            .optionalFlag("--version-code", "Version code (integer) to inject into the "
                          "AndroidManifest.xml if none is present",
                          &options.manifestFixerOptions.versionCodeDefault)
            .optionalFlag("--version-name", "Version name to inject into the AndroidManifest.xml "
                          "if none is present",
                          &options.manifestFixerOptions.versionNameDefault)
            .optionalSwitch("--static-lib", "Generate a static Android library",
                            &options.staticLib)
            .optionalSwitch("--no-static-lib-packages",
                            "Merge all library resources under the app's package",
                            &options.noStaticLibPackages)
            .optionalSwitch("--non-final-ids", "Generates R.java without the final modifier.\n"
                            "This is implied when --static-lib is specified.",
                            &options.generateNonFinalIds)
            .optionalFlag("--stable-ids", "File containing a list of name to ID mapping.",
                          &stableIdFilePath)
            .optionalFlag("--emit-ids", "Emit a file at the given path with a list of name to ID\n"
                          "mappings, suitable for use with --stable-ids.",
                          &options.resourceIdMapPath)
            .optionalFlag("--private-symbols", "Package name to use when generating R.java for "
                          "private symbols.\n"
                          "If not specified, public and private symbols will use the application's "
                          "package name",
                          &options.privateSymbols)
            .optionalFlag("--custom-package", "Custom Java package under which to generate R.java",
                          &options.customJavaPackage)
            .optionalFlagList("--extra-packages", "Generate the same R.java but with different "
                              "package names",
                              &extraJavaPackages)
            .optionalFlagList("--add-javadoc-annotation", "Adds a JavaDoc annotation to all "
                            "generated Java classes",
                            &options.javadocAnnotations)
            .optionalSwitch("--auto-add-overlay", "Allows the addition of new resources in "
                            "overlays without <add-resource> tags",
                            &options.autoAddOverlay)
            .optionalFlag("--rename-manifest-package", "Renames the package in AndroidManifest.xml",
                          &options.manifestFixerOptions.renameManifestPackage)
            .optionalFlag("--rename-instrumentation-target-package",
                          "Changes the name of the target package for instrumentation. Most useful "
                          "when used\nin conjunction with --rename-manifest-package",
                          &options.manifestFixerOptions.renameInstrumentationTargetPackage)
            .optionalFlagList("-0", "File extensions not to compress",
                              &options.extensionsToNotCompress)
            .optionalFlagList("--split", "Split resources matching a set of configs out to a "
                              "Split APK.\nSyntax: path/to/output.apk:<config>[,<config>[...]]",
                              &splitArgs)
            .optionalSwitch("-v", "Enables verbose logging",
                            &verbose);

    if (!flags.parse("aapt2 link", args, &std::cerr)) {
        return 1;
    }

    // Expand all argument-files passed into the command line. These start with '@'.
    std::vector<std::string> argList;
    for (const std::string& arg : flags.getArgs()) {
        if (util::stringStartsWith(arg, "@")) {
            const std::string path = arg.substr(1, arg.size() - 1);
            std::string error;
            if (!file::appendArgsFromFile(path, &argList, &error)) {
                context.getDiagnostics()->error(DiagMessage(path) << error);
                return 1;
            }
        } else {
            argList.push_back(arg);
        }
    }

    // Expand all argument-files passed to -R.
    for (const std::string& arg : overlayArgList) {
        if (util::stringStartsWith(arg, "@")) {
            const std::string path = arg.substr(1, arg.size() - 1);
            std::string error;
            if (!file::appendArgsFromFile(path, &options.overlayFiles, &error)) {
                context.getDiagnostics()->error(DiagMessage(path) << error);
                return 1;
            }
        } else {
            options.overlayFiles.push_back(arg);
        }
    }

    if (verbose) {
        context.setVerbose(verbose);
    }

    // Populate the set of extra packages for which to generate R.java.
    for (std::string& extraPackage : extraJavaPackages) {
        // A given package can actually be a colon separated list of packages.
        for (StringPiece package : util::split(extraPackage, ':')) {
            options.extraJavaPackages.insert(package.toString());
        }
    }

    if (productList) {
        for (StringPiece product : util::tokenize(productList.value(), ',')) {
            if (product != "" && product != "default") {
                options.products.insert(product.toString());
            }
        }
    }

    AxisConfigFilter filter;
    if (configs) {
        for (const StringPiece& configStr : util::tokenize(configs.value(), ',')) {
            ConfigDescription config;
            LocaleValue lv;
            if (lv.initFromFilterString(configStr)) {
                lv.writeTo(&config);
            } else if (!ConfigDescription::parse(configStr, &config)) {
                context.getDiagnostics()->error(
                        DiagMessage() << "invalid config '" << configStr << "' for -c option");
                return 1;
            }

            if (config.density != 0) {
                context.getDiagnostics()->warn(
                        DiagMessage() << "ignoring density '" << config << "' for -c option");
            } else {
                filter.addConfig(config);
            }
        }

        options.tableSplitterOptions.configFilter = &filter;
    }

    if (preferredDensity) {
        ConfigDescription preferredDensityConfig;
        if (!ConfigDescription::parse(preferredDensity.value(), &preferredDensityConfig)) {
            context.getDiagnostics()->error(DiagMessage() << "invalid density '"
                                            << preferredDensity.value()
                                            << "' for --preferred-density option");
            return 1;
        }

        // Clear the version that can be automatically added.
        preferredDensityConfig.sdkVersion = 0;

        if (preferredDensityConfig.diff(ConfigDescription::defaultConfig())
                != ConfigDescription::CONFIG_DENSITY) {
            context.getDiagnostics()->error(DiagMessage() << "invalid preferred density '"
                                            << preferredDensity.value() << "'. "
                                            << "Preferred density must only be a density value");
            return 1;
        }
        options.tableSplitterOptions.preferredDensity = preferredDensityConfig.density;
    }

    if (!options.staticLib && stableIdFilePath) {
        if (!loadStableIdMap(context.getDiagnostics(), stableIdFilePath.value(),
                             &options.stableIdMap)) {
            return 1;
        }
    }

    // Populate some default no-compress extensions that are already compressed.
    options.extensionsToNotCompress.insert({
            ".jpg", ".jpeg", ".png", ".gif",
            ".wav", ".mp2", ".mp3", ".ogg", ".aac",
            ".mpg", ".mpeg", ".mid", ".midi", ".smf", ".jet",
            ".rtttl", ".imy", ".xmf", ".mp4", ".m4a",
            ".m4v", ".3gp", ".3gpp", ".3g2", ".3gpp2",
            ".amr", ".awb", ".wma", ".wmv", ".webm", ".mkv"});

    // Parse the split parameters.
    for (const std::string& splitArg : splitArgs) {
        options.splitPaths.push_back({});
        options.splitConstraints.push_back({});
        if (!parseSplitParameter(splitArg, context.getDiagnostics(), &options.splitPaths.back(),
                                 &options.splitConstraints.back())) {
            return 1;
        }
    }

    // Turn off auto versioning for static-libs.
    if (options.staticLib) {
        options.noAutoVersion = true;
        options.noVersionVectors = true;
    }

    LinkCommand cmd(&context, options);
    return cmd.run(argList);
}

} // namespace aapt
