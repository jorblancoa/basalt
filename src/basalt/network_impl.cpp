#include <dirent.h>

#include "network_impl.hpp"

#include <gsl-lite/gsl-lite.hpp>
#include <rocksdb/db.h>
#include <rocksdb/filter_policy.h>
#include <rocksdb/slice_transform.h>
#include <rocksdb/table.h>
#include <spdlog/fmt/ostr.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

namespace basalt {

/**
 * \name RocksDB write policies helper
 * \{
 */

inline static const rocksdb::ReadOptions& default_read_options() {
    static const rocksdb::ReadOptions default_read_options;
    return default_read_options;
}

inline static const rocksdb::WriteOptions& write_options(bool commit) {
    if (commit) {
        static const rocksdb::WriteOptions sync_write = []() {
            rocksdb::WriteOptions eax;
            eax.sync = true;
            return eax;
        }();
        return sync_write;
    }
    static const rocksdb::WriteOptions async_write;
    return async_write;
}

/**
 * \}
 */

inline static const rocksdb::Options& db_options() {
    static const rocksdb::Options& db_options = []() {
        rocksdb::Options options;
        // FIXME set it on column family level
        options.prefix_extractor.reset(
            rocksdb::NewFixedPrefixTransform(1 + sizeof(node_t) + sizeof(node_id_t)));
        options.create_if_missing = true;
        return options;
    }();
    return db_options;
}

inline static const rocksdb::ColumnFamilyOptions& nodes_cfo() {
    static const rocksdb::ColumnFamilyOptions nodes_cfo = []() {
        rocksdb::ColumnFamilyOptions options;
        // optimize node prefix enumeration to look for all nodes of a particular
        // type
        options.prefix_extractor.reset(rocksdb::NewFixedPrefixTransform(1 + sizeof(node_t)));
        return options;
    }();
    return nodes_cfo;
}

inline static const rocksdb::ColumnFamilyOptions& connections_cfo() {
    static const rocksdb::ColumnFamilyOptions connections_cfo = []() {
        rocksdb::ColumnFamilyOptions options;
        // optimize node prefix enumeration to look for all edges of a particular
        // node
        options.prefix_extractor.reset(
            rocksdb::NewFixedPrefixTransform(1 + sizeof(node_t) + sizeof(node_id_t)));
        // Enable prefix bloom for mem tables
#if ROCKSDB_MAJOR == 4
        options.memtable_prefix_bloom_bits = 100000000;
        options.memtable_prefix_bloom_probes = 6;
#endif

        // Enable prefix bloom for SST files
        rocksdb::BlockBasedTableOptions table_options;
        table_options.filter_policy.reset(rocksdb::NewBloomFilterPolicy(10, true));
        options.table_factory.reset(NewBlockBasedTableFactory(table_options));

        return options;
    }();
    return connections_cfo;
}

inline static const std::string& nodes_cfn() {
    static const std::string nodes_cfn = rocksdb::kDefaultColumnFamilyName;
    return nodes_cfn;
}

inline static const std::string& connections_cfn() {
    static const std::string connections_cfn = "connections";
    return connections_cfn;
}

void network_impl_t::setup_db(const std::string& path) {
    std::unique_ptr<rocksdb::DB> db_ptr;
    {  // open db
        rocksdb::DB* db;
        rocksdb::Status status = rocksdb::DB::Open(db_options(), path, &db);
        db_ptr.reset(db);
        if (status.code() == rocksdb::Status::kInvalidArgument) {
            return;
        }
        to_status(status).raise_on_error();
    }

    std::vector<std::string> column_family_names;
    rocksdb::DB::ListColumnFamilies(db_options(), db_ptr->GetName(), &column_family_names);
    if (std::find(column_family_names.begin(), column_family_names.end(), nodes_cfn()) ==
        column_family_names.end()) {
        gsl::owner<rocksdb::ColumnFamilyHandle*> cf = nullptr;
        to_status(db_ptr->CreateColumnFamily(nodes_cfo(), nodes_cfn(), &cf)).raise_on_error();
        delete cf;
    }
    if (std::find(column_family_names.begin(), column_family_names.end(), connections_cfn()) ==
        column_family_names.end()) {
        gsl::owner<rocksdb::ColumnFamilyHandle*> cf = nullptr;
        to_status(db_ptr->CreateColumnFamily(connections_cfo(), connections_cfn(), &cf))
            .raise_on_error();
        delete cf;
    }
}

network_impl_t::network_impl_t(const std::string& path)
    : path_(path)
    , nodes_(*this)
    , connections_(*this) {
    rocksdb::DB* db;

    setup_db(path);

    std::vector<rocksdb::ColumnFamilyDescriptor> column_families;
    column_families.emplace_back(nodes_cfn(), nodes_cfo());
    column_families.emplace_back(connections_cfn(), connections_cfo());
    std::vector<rocksdb::ColumnFamilyHandle*> handles;
    handles.reserve(2);
    to_status(rocksdb::DB::Open(db_options(), path, column_families, &handles, &db))
        .raise_on_error();
    nodes.reset(handles[0]);
    connections.reset(handles[1]);

    db_.reset(db);
    mkdir(std::string(path + "/logs").c_str(), 0777);
    const std::string logger_name = "basalt[" + path + "]";
    logger_ = spdlog::get(logger_name);
    if (!logger_) {
        logger_ = spdlog::rotating_logger_mt(logger_name, path + "/logs/network.log", 1048576 * 5,
                                             3);
        logger_->info("creating or loading database at location: {}", path);
        logger_->set_level(spdlog::level::level_enum::trace);
    }
}

status_t network_impl_t::to_status(const rocksdb::Status& status) {
    return {static_cast<status_t::Code>(status.code()), status.ToString()};
}

///// nodes methods

status_t network_impl_t::nodes_insert(basalt::node_t type,
                                      basalt::node_id_t id,
                                      basalt::node_uid_t& node,
                                      bool commit) {
    logger_get()->debug("nodes_insert(type={}, id={}, commit={}", type, id, commit);
    graph::node_key_t key;
    node.first = type;
    node.second = id;
    graph::encode(node, key);
    return to_status(db_get()->Put(write_options(commit), nodes.get(),
                                   rocksdb::Slice(key.data(), key.size()), rocksdb::Slice()));
}

status_t network_impl_t::nodes_insert(basalt::node_t type,
                                      basalt::node_id_t id,
                                      const gsl::span<const char>& payload,
                                      basalt::node_uid_t& node,
                                      bool commit) {
    logger_get()->debug("nodes_insert(type={}, id={}, data_size={}, commit={}", type, id,
                        payload.size(), commit);
    graph::node_key_t key;
    node.first = type;
    node.second = id;
    graph::encode(node, key);
    return to_status(db_get()->Put(write_options(commit), nodes.get(),
                                   rocksdb::Slice(key.data(), key.size()),
                                   rocksdb::Slice(payload.data(), payload.size())));
}

status_t network_impl_t::nodes_has(const basalt::node_uid_t& node, bool& result) const {
    logger_get()->debug("nodes_has(node={})", node);
    graph::node_key_t key;
    graph::encode(node, key);
    const rocksdb::Slice slice(key.data(), key.size());

    std::string value;
    const auto& status = db_get()->Get(default_read_options(), nodes.get(), slice, &value);
    if (status.IsNotFound()) {
        result = false;
        return status_t::ok();
    }
    result = status.ok();
    return to_status(status);
}

status_t network_impl_t::nodes_get(const basalt::node_uid_t& node, std::string* value) {
    logger_get()->debug("nodes_get(node={}", node);
    graph::node_key_t key;
    graph::encode(node, key);
    const auto& status = db_get()->Get(default_read_options(), nodes.get(),
                                       rocksdb::Slice(key.data(), key.size()), value);
    if (status.IsNotFound()) {
        return status_t::error_missing_node(node);
    }
    return to_status(status);
}

status_t network_impl_t::nodes_erase(const node_uid_t& node, bool commit) {
    logger_get()->debug("nodes_erase(node={}, commit={})", node, commit);
    graph::node_key_t key;
    graph::encode(node, key);
    rocksdb::WriteBatch batch;
    const rocksdb::Slice slice(key.data(), key.size());
    batch.Delete(nodes.get(), slice);
    auto removed = 0ul;
    const auto status = connections_erase(batch, node, removed);
    if (!status) {
        return status;
    }
    return to_status(db_get()->Write(write_options(commit), &batch));
}

std::shared_ptr<node_iterator_impl> network_impl_t::node_iterator(std::size_t from) const {
    logger_get()->debug("node_iterator(from={})", from);
    return std::make_shared<node_iterator_impl>(db_get(), nodes.get(), "N", from);
}

///// connections methods

status_t network_impl_t::connections_insert(const node_uid_t& node1,
                                            const node_uid_t& node2,
                                            const gsl::span<const char>& payload,
                                            bool commit) {
    logger_get()->debug("connections_connect(node1={}, node2={}, commit={})", node1, node2, commit);
    {  // check presence of both nodes
        bool node_present = false;
        nodes_has(node1, node_present).raise_on_error();
        if (!node_present) {
            return status_t::error_missing_node(node1);
        }
        nodes_has(node2, node_present).raise_on_error();
        if (!node_present) {
            return status_t::error_missing_node(node2);
        }
    }
    if (node1 == node2) {
        return status_t::error_invalid_connection(node1, node2);
    }

    graph::connection_keys_t keys;
    graph::encode(node1, node2, keys);
    const rocksdb::Slice data_slice(payload.data(), payload.size());
    rocksdb::WriteBatch batch;

    for (const auto& key: keys) {
        batch.Put(connections.get(), rocksdb::Slice(key.data(), key.size()), data_slice);
    }
    return to_status(db_get()->Write(write_options(commit), &batch));
}

status_t network_impl_t::connections_insert(const node_uid_t& node,
                                            const node_uids_t& nodes,
                                            const std::vector<const char*>& data,
                                            const std::vector<std::size_t>& sizes,
                                            bool commit) {
    logger_get()->debug("connections_connect(node={}, nodes={}, commit={})", node, nodes, commit);
    {  // check presence of both nodes
        bool node_present = false;
        nodes_has(node, node_present).raise_on_error();
        if (!node_present) {
            return status_t::error_missing_node(node);
        }
        for (auto const& dest_node: nodes) {
            if (node == dest_node) {
                return status_t::error_invalid_connection(node, dest_node);
            }
            nodes_has(dest_node, node_present).raise_on_error();
            if (!node_present) {
                return status_t::error_missing_node(dest_node);
            }
        }
    }

    std::vector<graph::connection_keys_t> keys(nodes.size());
    rocksdb::WriteBatch batch;
    if (data.empty()) {
        for (auto i = 0u; i < nodes.size(); ++i) {
            graph::encode(node, nodes[i], keys[i]);
            for (const auto& key: keys[i]) {
                batch.Put(connections.get(), rocksdb::Slice(key.data(), key.size()),
                          rocksdb::Slice());
            }
        }
    } else {
        for (auto i = 0u; i < nodes.size(); ++i) {
            graph::encode(node, nodes[i], keys[i]);
            for (const auto& key: keys[i]) {
                batch.Put(connections.get(), rocksdb::Slice(key.data(), key.size()),
                          rocksdb::Slice(data[i], sizes[i]));
            }
        }
    }
    return to_status(db_get()->Write(write_options(commit), &batch));
}

status_t network_impl_t::connections_has(const basalt::node_uid_t& node1,
                                         const basalt::node_uid_t& node2,
                                         bool& res) const {
    logger_get()->debug("connections_has(node1={}, node2={})", node1, node2);
    graph::connection_key_t key;
    graph::encode(node1, node2, key);
    std::string value;
    const auto& status = db_get()->Get(default_read_options(), connections.get(),
                                       rocksdb::Slice(key.data(), key.size()), &value);
    if (status.IsNotFound()) {
        res = false;
        return status_t::ok();
    }
    res = status.ok();
    return to_status(status);
}

status_t network_impl_t::connections_get(const node_uid_t& node, node_uids_t& connections) const {
    logger_get()->debug("connections_get(node={})", node);
    graph::connection_key_prefix_t key;
    graph::encode_connection_prefix(node, key);
    const rocksdb::Slice slice(key.data(), key.size());
    auto iter = db_get()->NewIterator(default_read_options(), this->connections.get());
    iter->Seek(slice);
    while (iter->Valid()) {
        auto const& conn_key = iter->key();
        if (std::memcmp(key.data(), conn_key.data(), key.size()) != 0) {
            /// FIXME TCL prefix enumeration does not work, more keys
            /// are being returned by iterator.
            /// workaround: this test filter keys that do not have proper prefix
            iter->Next();
            continue;
        }
        node_uid_t dest;
        graph::decode_connection_dest(conn_key.data(), conn_key.size(), dest);
        connections.push_back(dest);
        iter->Next();
    }
    return to_status(iter->status());
}

status_t network_impl_t::connections_get(const node_uid_t& node,
                                         node_t filter,
                                         node_uids_t& connections) const {
    logger_get()->debug("connections_get(node={}, filter={}, connections={})", node, filter,
                        connections);
    graph::connection_key_type_prefix_t key;
    graph::encode_connection_prefix(node, filter, key);
    const rocksdb::Slice slice(key.data(), key.size());
    auto iter = db_get()->NewIterator(default_read_options(), this->connections.get());
    iter->Seek(slice);
    if (!iter->status().ok()) {
        return to_status(iter->status());
    }
    while (iter->Valid()) {
        auto const& conn_key = iter->key();
        if (std::memcmp(key.data(), conn_key.data(), key.size()) != 0) {
            /// FIXME TCL prefix enumeration does not work, more keys
            /// are being returned by iterator.
            /// workaround: this test filter keys that do not have proper prefix
            iter->Next();
            continue;
        }
        node_uid_t dest;
        graph::decode_connection_dest(conn_key.data(), conn_key.size(), dest);
        connections.push_back(dest);
        iter->Next();
    }
    return status_t::ok();
}

status_t network_impl_t::connections_erase(const node_uid_t& node1,
                                           const node_uid_t& node2,
                                           bool commit) {
    logger_get()->debug("connections_erase(node1={}, node2={}, commit={})", node1, node2, commit);

    graph::connection_keys_t keys;
    graph::encode(node1, node2, keys);
    rocksdb::WriteBatch batch;

    for (const auto& key: keys) {
        batch.Delete(connections.get(), rocksdb::Slice(key.data(), key.size()));
    }
    return to_status(db_get()->Write(write_options(commit), &batch));
}

status_t network_impl_t::connections_erase(rocksdb::WriteBatch& batch,
                                           const node_uid_t& node,
                                           size_t& removed) {
    graph::connection_key_prefix_t key;
    graph::encode_connection_prefix(node, key);
    const rocksdb::Slice slice(key.data(), key.size());

    auto iter = db_get()->NewIterator(default_read_options(), connections.get());
    iter->Seek(slice);

    // iterate over all connections
    auto connections = 0ul;
    while (iter->Valid()) {
        // remove the key
        auto const& conn_slice = iter->key();

        if (std::memcmp(key.data(), conn_slice.data(), key.size()) != 0) {
            /// FIXME TCL prefix enumeration does not work, more
            /// keys are being returned by iterator
            /// workaround: this test filter keys that do not have proper prefix
            iter->Next();
            continue;
        }
        batch.Delete(this->connections.get(), conn_slice);

        // remove the reverse connection
        graph::connection_key_t reversed_key;
        graph::encode_reversed_connection(conn_slice.data(), conn_slice.size(), reversed_key);
        const rocksdb::Slice reversed_slice(reversed_key.data(), reversed_key.size());
        batch.Delete(this->connections.get(), reversed_slice);
        ++connections;
        iter->Next();
    }
    const auto status = iter->status();
    if (status.ok()) {
        removed = connections;
    }
    return to_status(status);
}

status_t network_impl_t::connections_erase(const node_uid_t& node, size_t& removed, bool commit) {
    logger_get()->debug("connections_erase(node={}, commit={})", node, commit);
    rocksdb::WriteBatch batch;
    auto connections = 0ul;
    connections_erase(batch, node, connections).raise_on_error();
    auto const& status = to_status(db_get()->Write(write_options(commit), &batch));
    if (status) {
        removed = connections;
    } else {
        removed = 0;
    }
    return status;
}

status_t network_impl_t::connections_erase(const node_uid_t& node,
                                           node_t filter,
                                           size_t& removed,
                                           bool commit) {
    logger_get()->debug("connections_erase(node={}, filter={}, commit={})", node, filter, commit);
    graph::connection_key_type_prefix_t key;
    graph::encode_connection_prefix(node, filter, key);
    auto iter = db_get()->NewIterator(default_read_options());
    iter->Seek(rocksdb::Slice(key.data(), key.size()));

    // iterate over all connections
    rocksdb::WriteBatch batch;
    auto connections = 0ul;
    while (iter->Valid()) {
        auto const& conn_key = iter->key();
        if (std::memcmp(key.data(), conn_key.data(), key.size()) != 0) {
            /// FIXME TCL prefix enumeration does not work, more
            /// keys are being returned by iterator
            /// workaround: this test filter keys that do not have proper prefix
            iter->Next();
            continue;
        }
        batch.Delete(conn_key);

        // remove the reverse connection
        graph::connection_key_t reversed_key;
        graph::encode_reversed_connection(conn_key.data(), conn_key.size(), reversed_key);
        batch.Delete(rocksdb::Slice(reversed_key.data(), reversed_key.size()));
        ++connections;
        iter->Next();
    }
    auto status = iter->status();
    if (status.ok()) {
        status = db_get()->Write(write_options(commit), &batch);
    }
    if (status.ok()) {
        removed = connections;
    } else {
        removed = 0;
    }
    return to_status(status);
}

status_t network_impl_t::commit() {
    logger_get()->debug("commit()");
    to_status(db_get()->Flush(rocksdb::FlushOptions(), nodes.get())).raise_on_error();
    return to_status(db_get()->Flush(rocksdb::FlushOptions(), connections.get())).raise_on_error();
}

}  // namespace basalt