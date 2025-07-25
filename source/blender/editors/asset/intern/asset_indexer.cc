/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edasset
 */

#include <ctime>
#include <fstream>
#include <iomanip>
#include <optional>

#include "ED_asset_indexer.hh"

#include "DNA_ID.h"
#include "DNA_asset_types.h"

#include "BLI_fileops.h"
#include "BLI_hash.hh"
#include "BLI_linklist.h"
#include "BLI_listbase.h"
#include "BLI_path_utils.hh"
#include "BLI_serialize.hh"
#include "BLI_set.hh"
#include "BLI_string.h"
#include "BLI_string_ref.hh"
#include "BLI_string_utf8.h"

#include "AS_asset_catalog.hh"
#include "BKE_appdir.hh"
#include "BKE_asset.hh"
#include "BKE_idprop.hh"

#include "CLG_log.h"

#include <sstream>

static CLG_LogRef LOG = {"asset.index"};

namespace blender::ed::asset::index {

using namespace blender::asset_system;
using namespace blender::io::serialize;
using namespace blender::bke::idprop;

/**
 * \brief Indexer for asset libraries.
 *
 * Indexes are stored per input file. Each index can contain zero to multiple asset entries.
 * The indexes are grouped together per asset library. They are stored in
 * #BKE_appdir_folder_caches +
 * /asset-library-indices/<asset-library-hash>/<asset-index-hash>_<asset_file>.index.json.
 *
 * The structure of an index file is
 * \code
 * {
 *   "version": <file version number>,
 *   "entries": [{
 *     "name": "<asset name>",
 *     "catalog_id": "<catalog_id>",
 *     "catalog_name": "<catalog_name>",
 *     "description": "<description>",
 *     "author": "<author>",
 *     "copyright": "<copyright>",
 *     "license": "<license>",
 *     "tags": ["<tag>"],
 *     "properties": [..]
 *   }]
 * }
 * \endcode
 *
 * NOTE: entries, author, description, copyright, license, tags and properties are optional
 * attributes.
 *
 * NOTE: File browser uses name and idcode separate. Inside the index they are joined together like
 * #ID.name.
 * NOTE: File browser group name isn't stored in the index as it is a translatable name.
 */
constexpr StringRef ATTRIBUTE_VERSION("version");
constexpr StringRef ATTRIBUTE_ENTRIES("entries");
constexpr StringRef ATTRIBUTE_ENTRIES_NAME("name");
constexpr StringRef ATTRIBUTE_ENTRIES_CATALOG_ID("catalog_id");
constexpr StringRef ATTRIBUTE_ENTRIES_CATALOG_NAME("catalog_name");
constexpr StringRef ATTRIBUTE_ENTRIES_DESCRIPTION("description");
constexpr StringRef ATTRIBUTE_ENTRIES_AUTHOR("author");
constexpr StringRef ATTRIBUTE_ENTRIES_COPYRIGHT("copyright");
constexpr StringRef ATTRIBUTE_ENTRIES_LICENSE("license");
constexpr StringRef ATTRIBUTE_ENTRIES_TAGS("tags");
constexpr StringRef ATTRIBUTE_ENTRIES_PROPERTIES("properties");

/** Abstract class for #BlendFile and #AssetIndexFile. */
class AbstractFile {
 public:
  virtual ~AbstractFile() = default;

  virtual const char *get_file_path() const = 0;

  bool exists() const
  {
    return BLI_exists(this->get_file_path());
  }

  size_t get_file_size() const
  {
    return BLI_file_size(this->get_file_path());
  }
};

/**
 * \brief Reference to a blend file that can be indexed.
 */
class BlendFile : public AbstractFile {
  StringRefNull file_path_;

 public:
  BlendFile(StringRefNull file_path) : file_path_(file_path) {}

  uint64_t hash() const
  {
    DefaultHash<StringRefNull> hasher;
    return hasher(file_path_);
  }

  std::string get_filename() const
  {
    char filename[FILE_MAX];
    BLI_path_split_file_part(this->get_file_path(), filename, sizeof(filename));
    return std::string(filename);
  }

  const char *get_file_path() const override
  {
    return file_path_.c_str();
  }
};

/**
 * \brief add id + name to the attributes.
 *
 * NOTE: id and name are encoded like #ID.name
 */
static void add_id_name(DictionaryValue &result, const short idcode, const StringRefNull name)
{
  char idcode_prefix[2];
  /* Similar to `BKE_libblock_alloc`. */
  *((short *)idcode_prefix) = idcode;
  std::string name_with_idcode = std::string(idcode_prefix, sizeof(idcode_prefix)) + name;

  result.append_str(ATTRIBUTE_ENTRIES_NAME, name_with_idcode);
}

static void init_value_from_file_indexer_entry(DictionaryValue &result,
                                               const FileIndexerEntry *indexer_entry)
{
  const BLODataBlockInfo &datablock_info = indexer_entry->datablock_info;

  add_id_name(result, indexer_entry->idcode, datablock_info.name);

  const AssetMetaData &asset_data = *datablock_info.asset_data;
  result.append_str(ATTRIBUTE_ENTRIES_CATALOG_ID, CatalogID(asset_data.catalog_id).str());
  result.append_str(ATTRIBUTE_ENTRIES_CATALOG_NAME, asset_data.catalog_simple_name);

  if (const char *description = asset_data.description) {
    result.append_str(ATTRIBUTE_ENTRIES_DESCRIPTION, description);
  }
  if (const char *author = asset_data.author) {
    result.append_str(ATTRIBUTE_ENTRIES_AUTHOR, author);
  }
  if (const char *copyright = asset_data.copyright) {
    result.append_str(ATTRIBUTE_ENTRIES_COPYRIGHT, copyright);
  }
  if (const char *license = asset_data.license) {
    result.append_str(ATTRIBUTE_ENTRIES_LICENSE, license);
  }

  if (!BLI_listbase_is_empty(&asset_data.tags)) {
    ArrayValue &tags = *result.append_array(ATTRIBUTE_ENTRIES_TAGS);
    LISTBASE_FOREACH (AssetTag *, tag, &asset_data.tags) {
      tags.append_str(tag->name);
    }
  }

  if (const IDProperty *properties = asset_data.properties) {
    if (std::unique_ptr<Value> value = convert_to_serialize_values(properties)) {
      result.append(ATTRIBUTE_ENTRIES_PROPERTIES, std::move(value));
    }
  }
}

static void init_value_from_file_indexer_entries(DictionaryValue &result,
                                                 const FileIndexerEntries &indexer_entries)
{
  auto entries = std::make_shared<ArrayValue>();

  for (LinkNode *ln = indexer_entries.entries; ln; ln = ln->next) {
    const FileIndexerEntry *indexer_entry = static_cast<const FileIndexerEntry *>(ln->link);
    /* We also get non asset types (brushes, work-spaces), when browsing using the asset browser.
     */
    if (indexer_entry->datablock_info.asset_data == nullptr) {
      continue;
    }
    init_value_from_file_indexer_entry(*entries->append_dict(), indexer_entry);
  }

  /* When no entries to index, we should not store the entries attribute as this would make the
   * size bigger than the #MIN_FILE_SIZE_WITH_ENTRIES. */
  if (entries->elements().is_empty()) {
    return;
  }

  result.append(ATTRIBUTE_ENTRIES, entries);
}

static void init_indexer_entry_from_value(FileIndexerEntry &indexer_entry,
                                          const DictionaryValue &entry)
{
  const StringRef idcode_name = *entry.lookup_str(ATTRIBUTE_ENTRIES_NAME);

  indexer_entry.idcode = GS(idcode_name.data());

  idcode_name.substr(2).copy_utf8_truncated(indexer_entry.datablock_info.name);

  AssetMetaData *asset_data = BKE_asset_metadata_create();
  indexer_entry.datablock_info.asset_data = asset_data;
  indexer_entry.datablock_info.free_asset_data = true;

  if (const std::optional<StringRef> value = entry.lookup_str(ATTRIBUTE_ENTRIES_DESCRIPTION)) {
    asset_data->description = BLI_strdupn(value->data(), value->size());
  }
  if (const std::optional<StringRef> value = entry.lookup_str(ATTRIBUTE_ENTRIES_AUTHOR)) {
    asset_data->author = BLI_strdupn(value->data(), value->size());
  }
  if (const std::optional<StringRef> value = entry.lookup_str(ATTRIBUTE_ENTRIES_COPYRIGHT)) {
    asset_data->copyright = BLI_strdupn(value->data(), value->size());
  }
  if (const std::optional<StringRef> value = entry.lookup_str(ATTRIBUTE_ENTRIES_LICENSE)) {
    asset_data->license = BLI_strdupn(value->data(), value->size());
  }

  const StringRefNull catalog_name = *entry.lookup_str(ATTRIBUTE_ENTRIES_CATALOG_NAME);
  STRNCPY_UTF8(asset_data->catalog_simple_name, catalog_name.c_str());

  const StringRefNull catalog_id = *entry.lookup_str(ATTRIBUTE_ENTRIES_CATALOG_ID);
  asset_data->catalog_id = CatalogID(catalog_id);

  if (const ArrayValue *array_value = entry.lookup_array(ATTRIBUTE_ENTRIES_TAGS)) {
    for (const std::shared_ptr<Value> &item : array_value->elements()) {
      BKE_asset_metadata_tag_add(asset_data, item->as_string_value()->value().c_str());
    }
  }

  if (const std::shared_ptr<Value> *value = entry.lookup(ATTRIBUTE_ENTRIES_PROPERTIES)) {
    asset_data->properties = convert_from_serialize_value(**value);
  }
}

static int init_indexer_entries_from_value(FileIndexerEntries &indexer_entries,
                                           const DictionaryValue &value)
{
  const ArrayValue *entries = value.lookup_array(ATTRIBUTE_ENTRIES);
  BLI_assert(entries != nullptr);
  if (entries == nullptr) {
    return 0;
  }

  int num_entries_read = 0;
  for (const std::shared_ptr<Value> &element : entries->elements()) {
    FileIndexerEntry *entry = MEM_callocN<FileIndexerEntry>(__func__);
    init_indexer_entry_from_value(*entry, *element->as_dictionary_value());

    BLI_linklist_prepend(&indexer_entries.entries, entry);
    num_entries_read += 1;
  }

  return num_entries_read;
}

/**
 * \brief References the asset library directory.
 *
 * The #AssetLibraryIndex instance collects file indices that are existing before the actual
 * reading/updating starts. This way, the reading/updating can tag pre-existing files as used when
 * they are still needed. Remaining ones (indices that are not tagged as used) can be removed once
 * reading finishes.
 */
struct AssetLibraryIndex {
  struct PreexistingFileIndexInfo {
    bool is_used = false;
  };

  /**
   * File indices that are existing already before reading/updating performs changes. The key is
   * the absolute path. The value can store information like if the index is known to be used.
   *
   * Note that when deleting a file index (#delete_index_file()), it's also removed from here,
   * since it doesn't exist and isn't relevant to keep track of anymore.
   */
  Map<std::string /*path*/, PreexistingFileIndexInfo> preexisting_file_indices;

  /**
   * \brief Absolute path where the indices of `library` are stored.
   *
   * \note includes trailing directory separator.
   */
  std::string indices_base_path;

  std::string library_path;

  AssetLibraryIndex(const StringRef library_path) : library_path(library_path)
  {
    this->init_indices_base_path();
  }

  uint64_t hash() const
  {
    return get_default_hash(this->library_path);
  }

  StringRefNull get_library_file_path() const
  {
    return this->library_path;
  }

  /**
   * \brief Initializes #AssetLibraryIndex.indices_base_path.
   *
   * `BKE_appdir_folder_caches/asset-library-indices/<asset-library-name-hash>/`
   */
  void init_indices_base_path()
  {
    char index_path[FILE_MAX];
    BKE_appdir_folder_caches(index_path, sizeof(index_path));

    BLI_path_append(index_path, sizeof(index_path), "asset-library-indices");

    std::stringstream ss;
    ss << std::setfill('0') << std::setw(16) << std::hex << hash() << SEP_STR;
    BLI_path_append(index_path, sizeof(index_path), ss.str().c_str());

    this->indices_base_path = std::string(index_path);
  }

  /**
   * \return absolute path to the index file of the given `asset_file`.
   *
   * `{indices_base_path}/{asset-file_hash}_{asset-file-filename}.index.json`.
   */
  std::string index_file_path(const BlendFile &asset_file) const
  {
    std::stringstream ss;
    ss << this->indices_base_path;
    ss << std::setfill('0') << std::setw(16) << std::hex << asset_file.hash() << "_"
       << asset_file.get_filename() << ".index.json";
    return ss.str();
  }

  /**
   * Check for pre-existing index files to be able to track what is still used and what can be
   * removed. See #AssetLibraryIndex::preexisting_file_indices.
   */
  void collect_preexisting_file_indices()
  {
    const char *index_path = this->indices_base_path.c_str();
    if (!BLI_is_dir(index_path)) {
      return;
    }
    direntry *dir_entries = nullptr;
    const int dir_entries_num = BLI_filelist_dir_contents(index_path, &dir_entries);
    for (int i = 0; i < dir_entries_num; i++) {
      direntry *entry = &dir_entries[i];
      if (BLI_str_endswith(entry->relname, ".index.json")) {
        this->preexisting_file_indices.add_as(std::string(entry->path));
      }
    }

    BLI_filelist_free(dir_entries, dir_entries_num);
  }

  void mark_as_used(const std::string &filename)
  {
    PreexistingFileIndexInfo *preexisting = this->preexisting_file_indices.lookup_ptr(filename);
    if (preexisting) {
      preexisting->is_used = true;
    }
  }

  /**
   * Removes the file index from disk and #preexisting_file_indices (invalidating its iterators, so
   * don't call while iterating).
   * \return true if deletion was successful.
   */
  bool delete_file_index(const std::string &filename)
  {
    if (BLI_delete(filename.c_str(), false, false) == 0) {
      this->preexisting_file_indices.remove(filename);
      return true;
    }
    return false;
  }

  /**
   * A bug was creating empty index files for a while (see D16665). Remove empty index files from
   * this period, so they are regenerated.
   */
  /* Implemented further below. */
  int remove_broken_index_files();

  int remove_unused_index_files()
  {
    int num_files_deleted = 0;

    Set<StringRef> files_to_remove;

    for (auto preexisting_index : this->preexisting_file_indices.items()) {
      if (preexisting_index.value.is_used) {
        continue;
      }

      const std::string &file_path = preexisting_index.key;
      CLOG_DEBUG(&LOG, "Remove unused index file \"%s\".", file_path.c_str());
      files_to_remove.add(preexisting_index.key);
    }

    for (StringRef file_to_remove : files_to_remove) {
      if (delete_file_index(file_to_remove)) {
        num_files_deleted++;
      }
    }

    return num_files_deleted;
  }
};

/**
 * Instance of this class represents the contents of an asset index file.
 *
 * \code
 * {
 *    "version": {version},
 *    "entries": ...
 * }
 * \endcode
 */
struct AssetIndex {
  /**
   * \brief Version to store in new index files.
   *
   * Versions are written to each index file. When reading the version is checked against
   * `CURRENT_VERSION` to make sure we can use the index. Developer should increase
   * `CURRENT_VERSION` when changes are made to the structure of the stored index.
   */
  static const int CURRENT_VERSION = 1;

  /**
   * Version number to use when version couldn't be read from an index file.
   */
  const int UNKNOWN_VERSION = -1;

  /**
   * `io::serialize::Value` representing the contents of an index file.
   *
   * Value is used over #DictionaryValue as the contents of the index could be corrupted and
   * doesn't represent an object. In case corrupted files are detected the `get_version` would
   * return `UNKNOWN_VERSION`.
   */
  std::unique_ptr<Value> contents;

  /**
   * Constructor for when creating/updating an asset index file.
   * #AssetIndex.contents are filled from the given \p indexer_entries.
   */
  AssetIndex(const FileIndexerEntries &indexer_entries)
  {
    std::unique_ptr<DictionaryValue> root = std::make_unique<DictionaryValue>();
    root->append_int(ATTRIBUTE_VERSION, CURRENT_VERSION);
    init_value_from_file_indexer_entries(*root, indexer_entries);

    this->contents = std::move(root);
  }

  /**
   * Constructor when reading an asset index file.
   * #AssetIndex.contents are read from the given \p value.
   */
  AssetIndex(std::unique_ptr<Value> &value) : contents(std::move(value)) {}

  int get_version() const
  {
    const DictionaryValue *root = this->contents->as_dictionary_value();
    if (root == nullptr) {
      return UNKNOWN_VERSION;
    }
    const std::optional<int64_t> version_value = root->lookup_int(ATTRIBUTE_VERSION);
    return version_value.value_or(UNKNOWN_VERSION);
  }

  bool is_latest_version() const
  {
    return get_version() == CURRENT_VERSION;
  }

  /**
   * Extract the contents of this index into the given \p indexer_entries.
   *
   * \return The number of entries read from the given entries.
   */
  int extract_into(FileIndexerEntries &indexer_entries) const
  {
    const DictionaryValue *root = this->contents->as_dictionary_value();
    const int num_entries_read = init_indexer_entries_from_value(indexer_entries, *root);
    return num_entries_read;
  }
};

class AssetIndexFile : public AbstractFile {
 public:
  AssetLibraryIndex &library_index;
  /**
   * Asset index files with a size smaller than this attribute would be considered to not contain
   * any entries.
   */
  const size_t MIN_FILE_SIZE_WITH_ENTRIES = 32;
  std::string filename;

  AssetIndexFile(AssetLibraryIndex &library_index, StringRef index_file_path)
      : library_index(library_index), filename(index_file_path)
  {
  }

  AssetIndexFile(AssetLibraryIndex &library_index, BlendFile &asset_filename)
      : AssetIndexFile(library_index, library_index.index_file_path(asset_filename))
  {
  }

  void mark_as_used()
  {
    this->library_index.mark_as_used(this->filename);
  }

  const char *get_file_path() const override
  {
    return filename.c_str();
  }

  /**
   * Returns whether the index file is older than the given asset file.
   */
  bool is_older_than(const BlendFile &asset_file) const
  {
    return BLI_file_older(this->get_file_path(), asset_file.get_file_path());
  }

  /**
   * Check whether the index file contains entries without opening the file.
   */
  bool constains_entries() const
  {
    const size_t file_size = get_file_size();
    return file_size >= MIN_FILE_SIZE_WITH_ENTRIES;
  }

  std::unique_ptr<AssetIndex> read_contents() const
  {
    JsonFormatter formatter;
    std::ifstream is;
    is.open(this->filename);
    BLI_SCOPED_DEFER([&]() { is.close(); });

    std::unique_ptr<Value> read_data = formatter.deserialize(is);
    if (!read_data) {
      return nullptr;
    }

    return std::make_unique<AssetIndex>(read_data);
  }

  bool ensure_parent_path_exists() const
  {
    return BLI_file_ensure_parent_dir_exists(this->get_file_path());
  }

  void write_contents(AssetIndex &content)
  {
    JsonFormatter formatter;
    if (!ensure_parent_path_exists()) {
      CLOG_ERROR(&LOG, "Index not created: couldn't create folder \"%s\".", this->get_file_path());
      return;
    }

    std::ofstream os;
    os.open(this->filename, std::ios::out | std::ios::trunc);
    formatter.serialize(os, *content.contents);
    os.close();
  }
};

/* TODO(Julian): remove this after a short while. Just necessary for people who've been using alpha
 * builds from a certain period. */
int AssetLibraryIndex::remove_broken_index_files()
{
  Set<StringRef> files_to_remove;

  for (const std::string &index_path : this->preexisting_file_indices.keys()) {
    AssetIndexFile index_file(*this, index_path);

    /* Bug was causing empty index files, so non-empty ones can be skipped. */
    if (index_file.constains_entries()) {
      continue;
    }

    /* Use the file modification time stamp to attempt to remove empty index files from a
     * certain period (when the bug was in there). Starting from a day before the bug was
     * introduced until a day after the fix should be enough to mitigate possible local time
     * zone issues. */

    std::tm tm_from{};
    tm_from.tm_year = 2022 - 1900; /* 2022 */
    tm_from.tm_mon = 11 - 1;       /* November */
    tm_from.tm_mday = 8;           /* Day before bug was introduced. */
    std::tm tm_to{};
    tm_from.tm_year = 2022 - 1900; /* 2022 */
    tm_from.tm_mon = 12 - 1;       /* December */
    tm_from.tm_mday = 3;           /* Day after fix. */
    std::time_t timestamp_from = std::mktime(&tm_from);
    std::time_t timestamp_to = std::mktime(&tm_to);
    BLI_stat_t stat = {};
    if (BLI_stat(index_file.get_file_path(), &stat) == -1) {
      continue;
    }
    if (IN_RANGE(stat.st_mtime, timestamp_from, timestamp_to)) {
      CLOG_DEBUG(&LOG, "Remove potentially broken index file \"%s\".", index_path.c_str());
      files_to_remove.add(index_path);
    }
  }

  int num_files_deleted = 0;
  for (StringRef filepath : files_to_remove) {
    if (delete_file_index(filepath)) {
      num_files_deleted++;
    }
  }

  return num_files_deleted;
}

static eFileIndexerResult read_index(const char *filename,
                                     FileIndexerEntries *entries,
                                     int *r_read_entries_len,
                                     void *user_data)
{
  AssetLibraryIndex &library_index = *static_cast<AssetLibraryIndex *>(user_data);
  BlendFile asset_file(filename);
  AssetIndexFile asset_index_file(library_index, asset_file);

  if (!asset_index_file.exists()) {
    return FILE_INDEXER_NEEDS_UPDATE;
  }

  /* Mark index as used, even when it will be recreated. When not done it would remove the index
   * when the indexing has finished (see `AssetLibraryIndex.remove_unused_index_files`), thereby
   * removing the newly created index.
   */
  asset_index_file.mark_as_used();

  if (asset_index_file.is_older_than(asset_file)) {
    CLOG_DEBUG(
        &LOG,
        "Asset index file \"%s\" needs to be refreshed as it is older than the asset file \"%s\".",
        asset_index_file.filename.c_str(),
        filename);
    return FILE_INDEXER_NEEDS_UPDATE;
  }

  if (!asset_index_file.constains_entries()) {
    CLOG_DEBUG(&LOG,
               "Asset file index is to small to contain any entries. \"%s\"",
               asset_index_file.filename.c_str());
    *r_read_entries_len = 0;
    return FILE_INDEXER_ENTRIES_LOADED;
  }

  std::unique_ptr<AssetIndex> contents = asset_index_file.read_contents();
  if (!contents) {
    CLOG_DEBUG(&LOG, "Asset file index is ignored; failed to read contents.");
    return FILE_INDEXER_NEEDS_UPDATE;
  }

  if (!contents->is_latest_version()) {
    CLOG_DEBUG(&LOG,
               "Asset file index is ignored; expected version %d but file is version %d \"%s\".",
               AssetIndex::CURRENT_VERSION,
               contents->get_version(),
               asset_index_file.filename.c_str());
    return FILE_INDEXER_NEEDS_UPDATE;
  }

  const int read_entries_len = contents->extract_into(*entries);
  CLOG_INFO(&LOG, "Read %d entries for \"%s\".", read_entries_len, filename);
  *r_read_entries_len = read_entries_len;

  return FILE_INDEXER_ENTRIES_LOADED;
}

static void update_index(const char *filename, FileIndexerEntries *entries, void *user_data)
{
  AssetLibraryIndex &library_index = *static_cast<AssetLibraryIndex *>(user_data);
  BlendFile asset_file(filename);
  AssetIndexFile asset_index_file(library_index, asset_file);
  CLOG_INFO(&LOG,
            "Update for \"%s\" store index in \"%s\".",
            asset_file.get_file_path(),
            asset_index_file.get_file_path());

  AssetIndex content(*entries);
  asset_index_file.write_contents(content);
}

static void *init_user_data(const char *root_directory, size_t root_directory_maxncpy)
{
  AssetLibraryIndex *library_index = MEM_new<AssetLibraryIndex>(
      __func__, StringRef(root_directory, BLI_strnlen(root_directory, root_directory_maxncpy)));
  library_index->collect_preexisting_file_indices();
  library_index->remove_broken_index_files();
  return library_index;
}

static void free_user_data(void *user_data)
{
  MEM_delete((AssetLibraryIndex *)user_data);
}

static void filelist_finished(void *user_data)
{
  AssetLibraryIndex &library_index = *static_cast<AssetLibraryIndex *>(user_data);
  const int num_indices_removed = library_index.remove_unused_index_files();
  if (num_indices_removed > 0) {
    CLOG_INFO(&LOG, "Removed %d unused indices.", num_indices_removed);
  }
}

constexpr FileIndexerType asset_indexer()
{
  FileIndexerType indexer = {nullptr};
  indexer.read_index = read_index;
  indexer.update_index = update_index;
  indexer.init_user_data = init_user_data;
  indexer.free_user_data = free_user_data;
  indexer.filelist_finished = filelist_finished;
  return indexer;
}

const FileIndexerType file_indexer_asset = asset_indexer();

}  // namespace blender::ed::asset::index
