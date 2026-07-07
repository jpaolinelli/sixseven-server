#include "sixseven/executor/external_sort.h"

#include "sixseven/common/coercion.h"
#include "sixseven/executor/expr_evaluator.h"

#include <algorithm>
#include <cstring>

namespace sixseven {

// =============================================================================
// Construction / destruction
// =============================================================================

ExternalSortOperator::ExternalSortOperator(std::unique_ptr<Iterator> child,
                                           std::vector<SortKey> keys,
                                           const BoundStatement& bound,
                                           size_t work_mem_bytes,
                                           size_t max_merge_width,
                                           std::filesystem::path temp_dir)
    : child_(std::move(child)), keys_(std::move(keys)), bound_(bound),
      work_mem_bytes_(work_mem_bytes), max_merge_width_(std::max<size_t>(2, max_merge_width)),
      temp_dir_(std::move(temp_dir)) {
    if (temp_dir_.empty()) {
        temp_dir_ = std::filesystem::temp_directory_path() / "sixseven_sort";
    }
}

ExternalSortOperator::~ExternalSortOperator() {
    cleanup_temp_files();
}

// =============================================================================
// Iterator interface
// =============================================================================

Result<void> ExternalSortOperator::do_open() {
    auto res = child_->open();
    if (!res) {
        return res;
    }

    sorted_.clear();
    cursor_ = 0;
    in_memory_mode_ = false;
    heap_.clear();
    readers_.clear();

    // Phase 1: read all child tuples, generating sorted runs.
    auto gen = generate_runs();
    if (!gen) {
        cleanup_temp_files();
        return gen;
    }

    if (in_memory_mode_) {
        // Small dataset — sort was done in memory during generate_runs().
        return ok();
    }

    // Phase 1.5: reduce run count if necessary.
    auto red = reduce_runs();
    if (!red) {
        cleanup_temp_files();
        return red;
    }

    // Phase 2 setup: open run readers and prime the merge heap.
    auto merge = setup_final_merge();
    if (!merge) {
        cleanup_temp_files();
        return merge;
    }

    return ok();
}

Result<std::optional<Tuple>> ExternalSortOperator::do_next() {
    if (in_memory_mode_) {
        if (cursor_ >= sorted_.size()) {
            return ok(std::optional<Tuple>(std::nullopt));
        }
        return ok(std::optional<Tuple>(std::move(sorted_[cursor_++])));
    }

    // External merge: pull from the min-heap.
    if (heap_.empty()) {
        return ok(std::optional<Tuple>(std::nullopt));
    }

    auto entry = heap_pop();
    Tuple result = std::move(entry.tuple);
    size_t ri = entry.run_index;

    // Refill from the same run.
    auto next_tuple = read_tuple(readers_[ri].stream);
    if (!next_tuple) {
        return make_error(next_tuple.error().code, next_tuple.error().message);
    }
    if (next_tuple->has_value()) {
        heap_push({std::move(next_tuple->value()), ri});
    }

    return ok(std::optional<Tuple>(std::move(result)));
}

void ExternalSortOperator::do_close() {
    sorted_.clear();
    cursor_ = 0;
    heap_.clear();

    for (auto& r : readers_) {
        if (r.stream.is_open()) {
            r.stream.close();
        }
    }
    readers_.clear();

    cleanup_temp_files();
    child_->close();
}

const OutputSchema& ExternalSortOperator::output_schema() const {
    return child_->output_schema();
}

std::string ExternalSortOperator::plan_node_name() const {
    return "External Sort";
}

std::string ExternalSortOperator::plan_node_detail() const {
    return "";
}

std::vector<const Iterator*> ExternalSortOperator::plan_children() const {
    return {child_.get()};
}

std::vector<Iterator*> ExternalSortOperator::plan_children_mutable() {
    return {child_.get()};
}

// =============================================================================
// Memory estimation
// =============================================================================

size_t ExternalSortOperator::estimate_tuple_size(const Tuple& tuple) {
    size_t size = sizeof(Tuple);
    for (const auto& v : tuple.values) {
        size += sizeof(Value);
        if (!v.is_null()) {
            // Variable-length types carry heap-allocated data beyond sizeof(Value).
            switch (v.type_id()) {
            case TypeId::STRING:
                size += v.as_string().capacity();
                break;
            case TypeId::BLOB:
                size += v.as_blob().capacity();
                break;
            case TypeId::JSON:
                size += v.as_json().data.capacity();
                break;
            case TypeId::EMBEDDING:
                size += v.as_embedding().capacity() * sizeof(float);
                break;
            case TypeId::PATH:
                size += v.as_path().steps.capacity() * sizeof(PathStep);
                break;
            default:
                break;
            }
        }
    }
    return size;
}

// =============================================================================
// Phase 1 — Run generation
// =============================================================================

Result<void> ExternalSortOperator::generate_runs() {
    std::vector<Tuple> buffer;
    size_t mem_used = 0;

    while (true) {
        auto row = child_->next();
        if (!row) {
            return make_error(row.error().code, row.error().message);
        }
        if (!row->has_value()) {
            break;
        }

        auto& tuple = row->value();
        size_t tuple_size = estimate_tuple_size(tuple);
        mem_used += tuple_size;
        buffer.push_back(std::move(tuple));

        // Flush when we exceed work_mem (but always keep at least one tuple).
        if (mem_used >= work_mem_bytes_ && buffer.size() > 1) {
            auto fl = flush_run(buffer);
            if (!fl) {
                return fl;
            }
            buffer.clear();
            mem_used = 0;
        }
    }

    // If no runs were flushed, the entire dataset fits in memory.
    if (run_files_.empty()) {
        in_memory_mode_ = true;

        std::stable_sort(buffer.begin(), buffer.end(), [this](const Tuple& a, const Tuple& b) {
            return compare_tuples(a, b);
        });

        sorted_ = std::move(buffer);
        cursor_ = 0;
        return ok();
    }

    // Flush remaining tuples as the last run.
    if (!buffer.empty()) {
        auto fl = flush_run(buffer);
        if (!fl) {
            return fl;
        }
    }

    return ok();
}

Result<void> ExternalSortOperator::flush_run(std::vector<Tuple>& buffer) {
    // Sort the buffer in memory.
    std::stable_sort(buffer.begin(), buffer.end(), [this](const Tuple& a, const Tuple& b) {
        return compare_tuples(a, b);
    });

    // Write sorted tuples to a temp file.
    auto path = next_temp_path();
    std::filesystem::create_directories(path.parent_path());

    std::ofstream out(path, std::ios::binary);
    if (!out) {
        return make_error(StatusCode::IO_ERROR,
                          "failed to create temp sort file: " + path.string());
    }

    // Write tuple count header.
    auto count = static_cast<uint32_t>(buffer.size());
    out.write(reinterpret_cast<const char*>(&count), sizeof(count));

    for (const auto& tuple : buffer) {
        auto wr = write_tuple(out, tuple);
        if (!wr) {
            out.close();
            std::filesystem::remove(path);
            return wr;
        }
    }

    out.close();
    run_files_.push_back(std::move(path));
    return ok();
}

// =============================================================================
// Phase 1.5 — Multi-pass merge (reduce run count)
// =============================================================================

Result<void> ExternalSortOperator::reduce_runs() {
    while (run_files_.size() > max_merge_width_) {
        std::vector<std::filesystem::path> new_runs;
        size_t i = 0;

        while (i < run_files_.size()) {
            size_t end = std::min(i + max_merge_width_, run_files_.size());
            auto output_path = next_temp_path();

            auto res = merge_runs_to_file(i, end, output_path);
            if (!res) {
                return res;
            }
            new_runs.push_back(std::move(output_path));
            i = end;
        }

        // Remove old run files.
        for (auto& f : run_files_) {
            std::filesystem::remove(f);
        }
        run_files_ = std::move(new_runs);
    }
    return ok();
}

Result<void> ExternalSortOperator::merge_runs_to_file(size_t begin,
                                                      size_t end,
                                                      const std::filesystem::path& output) {

    // Open readers for the run subset.
    std::vector<std::ifstream> inputs;
    for (size_t i = begin; i < end; ++i) {
        std::ifstream in(run_files_[i], std::ios::binary);
        if (!in) {
            return make_error(StatusCode::IO_ERROR,
                              "failed to open run file: " + run_files_[i].string());
        }
        // Skip the tuple count header.
        uint32_t count = 0;
        in.read(reinterpret_cast<char*>(&count), sizeof(count));
        inputs.push_back(std::move(in));
    }

    // Build a local min-heap.
    struct Entry {
        Tuple tuple;
        size_t idx;
    };

    auto greater = [this](const Entry& a, const Entry& b) {
        return compare_tuples(b.tuple, a.tuple);
    };

    std::vector<Entry> local_heap;
    for (size_t i = 0; i < inputs.size(); ++i) {
        auto t = read_tuple(inputs[i]);
        if (!t) {
            return make_error(t.error().code, t.error().message);
        }
        if (t->has_value()) {
            local_heap.push_back({std::move(t->value()), i});
        }
    }
    std::make_heap(local_heap.begin(), local_heap.end(), greater);

    // Open output file and write merged tuples.
    std::filesystem::create_directories(output.parent_path());
    std::ofstream out(output, std::ios::binary);
    if (!out) {
        return make_error(StatusCode::IO_ERROR,
                          "failed to create merge output: " + output.string());
    }

    // Write a placeholder count (we'll patch it later).
    uint32_t total_count = 0;
    auto count_pos = out.tellp();
    out.write(reinterpret_cast<const char*>(&total_count), sizeof(total_count));

    while (!local_heap.empty()) {
        std::pop_heap(local_heap.begin(), local_heap.end(), greater);
        auto entry = std::move(local_heap.back());
        local_heap.pop_back();

        auto wr = write_tuple(out, entry.tuple);
        if (!wr) {
            return wr;
        }
        ++total_count;

        // Refill from the same input.
        auto next_t = read_tuple(inputs[entry.idx]);
        if (!next_t) {
            return make_error(next_t.error().code, next_t.error().message);
        }
        if (next_t->has_value()) {
            local_heap.push_back({std::move(next_t->value()), entry.idx});
            std::push_heap(local_heap.begin(), local_heap.end(), greater);
        }
    }

    // Patch the count.
    out.seekp(count_pos);
    out.write(reinterpret_cast<const char*>(&total_count), sizeof(total_count));
    out.close();

    return ok();
}

// =============================================================================
// Phase 2 — Final k-way merge setup
// =============================================================================

Result<void> ExternalSortOperator::setup_final_merge() {
    readers_.clear();
    heap_.clear();

    for (size_t i = 0; i < run_files_.size(); ++i) {
        RunReader rr;
        rr.path = run_files_[i];
        rr.stream.open(rr.path, std::ios::binary);
        if (!rr.stream) {
            return make_error(StatusCode::IO_ERROR,
                              "failed to open run for merge: " + rr.path.string());
        }
        // Skip the tuple count header.
        uint32_t count = 0;
        rr.stream.read(reinterpret_cast<char*>(&count), sizeof(count));
        readers_.push_back(std::move(rr));
    }

    // Prime the heap with the first tuple from each run.
    for (size_t i = 0; i < readers_.size(); ++i) {
        auto t = read_tuple(readers_[i].stream);
        if (!t) {
            return make_error(t.error().code, t.error().message);
        }
        if (t->has_value()) {
            heap_push({std::move(t->value()), i});
        }
    }

    return ok();
}

// =============================================================================
// Tuple comparison
// =============================================================================

bool ExternalSortOperator::compare_tuples(const Tuple& a, const Tuple& b) const {
    const auto& schema = child_->output_schema();
    for (const auto& key : keys_) {
        auto va = evaluate_expr(*key.expr, a, schema, bound_);
        auto vb = evaluate_expr(*key.expr, b, schema, bound_);
        if (!va || !vb) {
            return false;
        }
        if (va->is_null() && vb->is_null()) {
            continue;
        }
        if (va->is_null()) {
            return key.direction == SortDirection::DESC;
        }
        if (vb->is_null()) {
            return key.direction == SortDirection::ASC;
        }
        auto cmp = compare(*va, *vb);
        if (!cmp) {
            return false;
        }
        if (*cmp == std::strong_ordering::less) {
            return key.direction == SortDirection::ASC;
        }
        if (*cmp == std::strong_ordering::greater) {
            return key.direction == SortDirection::DESC;
        }
    }
    return false;
}

// =============================================================================
// Manual min-heap (compare_tuples returns true when a < b)
// =============================================================================

void ExternalSortOperator::heap_push(HeapEntry entry) {
    heap_.push_back(std::move(entry));
    sift_up(heap_.size() - 1);
}

ExternalSortOperator::HeapEntry ExternalSortOperator::heap_pop() {
    auto top = std::move(heap_[0]);
    heap_[0] = std::move(heap_.back());
    heap_.pop_back();
    if (!heap_.empty()) {
        sift_down(0);
    }
    return top;
}

void ExternalSortOperator::sift_down(size_t i) {
    size_t n = heap_.size();
    while (true) {
        size_t smallest = i;
        size_t left = (2 * i) + 1;
        size_t right = (2 * i) + 2;

        if (left < n && compare_tuples(heap_[left].tuple, heap_[smallest].tuple)) {
            smallest = left;
        }
        if (right < n && compare_tuples(heap_[right].tuple, heap_[smallest].tuple)) {
            smallest = right;
        }
        if (smallest == i) {
            break;
        }
        std::swap(heap_[i], heap_[smallest]);
        i = smallest;
    }
}

void ExternalSortOperator::sift_up(size_t i) {
    while (i > 0) {
        size_t parent = (i - 1) / 2;
        if (compare_tuples(heap_[i].tuple, heap_[parent].tuple)) {
            std::swap(heap_[i], heap_[parent]);
            i = parent;
        } else {
            break;
        }
    }
}

// =============================================================================
// Tuple serialization
// =============================================================================

namespace {

template <typename T>
void write_pod(std::ofstream& out, const T& val) {
    out.write(reinterpret_cast<const char*>(&val), sizeof(T));
}

template <typename T>
T read_pod(std::ifstream& in) {
    T val{};
    in.read(reinterpret_cast<char*>(&val), sizeof(T));
    return val;
}

void write_bytes(std::ofstream& out, const void* data, size_t len) {
    auto size = static_cast<uint32_t>(len);
    write_pod(out, size);
    out.write(static_cast<const char*>(data), static_cast<std::streamsize>(len));
}

// Forward declarations: write_value_payload()/read_value_payload() write/read
// a Value's non-null payload (tid tag + type-specific data). Used both for
// top-level Tuple values and, since GDB-1292, for PathStep::node_pk() (which
// is a Value rather than a fixed-width int64_t and may itself be any
// PK-eligible type, including STRING). A node PK is never itself PATH-typed
// in practice, so no unbounded recursion occurs.
void write_value_payload(std::ofstream& out, const Value& v);
Value read_value_payload(std::ifstream& in, uint8_t tid);

void write_value_payload(std::ofstream& out, const Value& v) {
    switch (v.type_id()) {
    case TypeId::INT8:
        write_pod(out, v.as_int8());
        break;
    case TypeId::INT16:
        write_pod(out, v.as_int16());
        break;
    case TypeId::INT32:
        write_pod(out, v.as_int32());
        break;
    case TypeId::INT64:
        write_pod(out, v.as_int64());
        break;
    case TypeId::UINT8:
        write_pod(out, v.as_uint8());
        break;
    case TypeId::UINT16:
        write_pod(out, v.as_uint16());
        break;
    case TypeId::UINT32:
        write_pod(out, v.as_uint32());
        break;
    case TypeId::UINT64:
        write_pod(out, v.as_uint64());
        break;
    case TypeId::FLOAT32:
        write_pod(out, v.as_float32());
        break;
    case TypeId::FLOAT64:
        write_pod(out, v.as_float64());
        break;
    case TypeId::DECIMAL:
        write_pod(out, v.as_decimal().hi);
        write_pod(out, v.as_decimal().lo);
        break;
    case TypeId::BOOL:
        write_pod(out, v.as_bool());
        break;
    case TypeId::STRING: {
        const auto& s = v.as_string();
        write_bytes(out, s.data(), s.size());
        break;
    }
    case TypeId::BLOB: {
        const auto& b = v.as_blob();
        write_bytes(out, b.data(), b.size());
        break;
    }
    case TypeId::DATE:
        write_pod(out, v.as_date().days_since_epoch);
        break;
    case TypeId::TIME:
        write_pod(out, v.as_time().microseconds);
        break;
    case TypeId::TIMESTAMP:
        write_pod(out, v.as_timestamp().microseconds);
        break;
    case TypeId::INTERVAL:
        write_pod(out, v.as_interval().months);
        write_pod(out, v.as_interval().microseconds);
        break;
    case TypeId::POINT:
        write_pod(out, v.as_point().x);
        write_pod(out, v.as_point().y);
        break;
    case TypeId::JSON: {
        const auto& j = v.as_json().data;
        write_bytes(out, j.data(), j.size());
        break;
    }
    case TypeId::UUID: {
        const auto& u = v.as_uuid();
        out.write(reinterpret_cast<const char*>(u.data()), 16);
        break;
    }
    case TypeId::EMBEDDING: {
        const auto& e = v.as_embedding();
        auto dim = static_cast<uint32_t>(e.size());
        write_pod(out, dim);
        out.write(reinterpret_cast<const char*>(e.data()),
                  static_cast<std::streamsize>(dim * sizeof(float)));
        break;
    }
    case TypeId::PATH: {
        const auto& p = v.as_path();
        write_pod(out, p.total_weight);
        auto count = static_cast<uint32_t>(p.steps.size());
        write_pod(out, count);
        for (const auto& step : p.steps) {
            const Value& pk = step.node_pk();
            uint8_t null_flag = pk.is_null() ? 0 : 1;
            write_pod(out, null_flag);
            if (null_flag != 0) {
                auto tid = static_cast<uint8_t>(pk.data().index());
                write_pod(out, tid);
                write_value_payload(out, pk);
            }
            write_pod(out, step.edge_id);
        }
        break;
    }
    }
}

Value read_value_payload(std::ifstream& in, uint8_t tid) {
    switch (tid) {
    case 1: // INT8
        return Value(read_pod<int8_t>(in));
    case 2: // INT16
        return Value(read_pod<int16_t>(in));
    case 3: // INT32
        return Value(read_pod<int32_t>(in));
    case 4: // INT64
        return Value(read_pod<int64_t>(in));
    case 5: // UINT8
        return Value(read_pod<uint8_t>(in));
    case 6: // UINT16
        return Value(read_pod<uint16_t>(in));
    case 7: // UINT32
        return Value(read_pod<uint32_t>(in));
    case 8: // UINT64
        return Value(read_pod<uint64_t>(in));
    case 9: // FLOAT32
        return Value(read_pod<float>(in));
    case 10: // FLOAT64
        return Value(read_pod<double>(in));
    case 11: { // DECIMAL
        Decimal128 d;
        d.hi = read_pod<int64_t>(in);
        d.lo = read_pod<uint64_t>(in);
        return Value(d);
    }
    case 12: // BOOL
        return Value(read_pod<bool>(in));
    case 13: { // STRING
        auto len = read_pod<uint32_t>(in);
        std::string s(len, '\0');
        in.read(s.data(), len);
        return Value(std::move(s));
    }
    case 14: { // BLOB
        auto len = read_pod<uint32_t>(in);
        Blob b(len);
        in.read(reinterpret_cast<char*>(b.data()), len);
        return Value(std::move(b));
    }
    case 15: { // DATE
        Date d;
        d.days_since_epoch = read_pod<int32_t>(in);
        return Value(d);
    }
    case 16: { // TIME
        Time t;
        t.microseconds = read_pod<int64_t>(in);
        return Value(t);
    }
    case 17: { // TIMESTAMP
        Timestamp ts;
        ts.microseconds = read_pod<int64_t>(in);
        return Value(ts);
    }
    case 18: { // INTERVAL
        Interval iv;
        iv.months = read_pod<int64_t>(in);
        iv.microseconds = read_pod<int64_t>(in);
        return Value(iv);
    }
    case 19: { // POINT
        Point pt;
        pt.x = read_pod<double>(in);
        pt.y = read_pod<double>(in);
        return Value(pt);
    }
    case 20: { // JSON
        auto len = read_pod<uint32_t>(in);
        std::string s(len, '\0');
        in.read(s.data(), len);
        return Value(JsonString{std::move(s)});
    }
    case 21: { // UUID
        Uuid u{};
        in.read(reinterpret_cast<char*>(u.data()), 16);
        return Value(u);
    }
    case 22: { // EMBEDDING
        auto dim = read_pod<uint32_t>(in);
        Embedding e(dim);
        in.read(reinterpret_cast<char*>(e.data()),
                static_cast<std::streamsize>(dim * sizeof(float)));
        return Value(std::move(e));
    }
    case 23: { // PATH
        auto total_weight = read_pod<double>(in);
        auto count = read_pod<uint32_t>(in);
        Path p;
        p.total_weight = total_weight;
        p.steps.reserve(count);
        for (uint32_t s = 0; s < count; ++s) {
            uint8_t null_flag = read_pod<uint8_t>(in);
            Value pk;
            if (null_flag != 0) {
                auto pk_tid = read_pod<uint8_t>(in);
                pk = read_value_payload(in, pk_tid);
            }
            int64_t edge_id = read_pod<int64_t>(in);
            p.steps.push_back({std::move(pk), edge_id});
        }
        return Value(std::move(p));
    }
    default:
        return Value::make_null();
    }
}

} // namespace

Result<void> ExternalSortOperator::write_tuple(std::ofstream& out, const Tuple& tuple) {
    auto num_values = static_cast<uint32_t>(tuple.values.size());
    write_pod(out, num_values);

    // RID
    uint8_t has_rid = tuple.rid.has_value() ? 1 : 0;
    write_pod(out, has_rid);
    if (has_rid != 0) {
        write_pod(out, tuple.rid->page_id);
        write_pod(out, tuple.rid->slot_id);
    }

    for (const auto& v : tuple.values) {
        uint8_t null_flag = v.is_null() ? 0 : 1;
        write_pod(out, null_flag);
        if (null_flag == 0) {
            continue;
        }

        auto tid = static_cast<uint8_t>(v.data().index());
        write_pod(out, tid);
        write_value_payload(out, v);
    }

    if (!out) {
        return make_error(StatusCode::IO_ERROR, "write error during tuple serialization");
    }
    return ok();
}

Result<std::optional<Tuple>> ExternalSortOperator::read_tuple(std::ifstream& in) {
    uint32_t num_values = 0;
    in.read(reinterpret_cast<char*>(&num_values), sizeof(num_values));
    if (in.eof() || in.gcount() == 0) {
        return ok(std::optional<Tuple>(std::nullopt));
    }
    if (!in) {
        return make_error(StatusCode::IO_ERROR, "read error during tuple deserialization");
    }

    Tuple tuple;

    // RID
    auto has_rid = read_pod<uint8_t>(in);
    if (has_rid != 0) {
        RID rid;
        rid.page_id = read_pod<PageId>(in);
        rid.slot_id = read_pod<SlotId>(in);
        tuple.rid = rid;
    }

    tuple.values.resize(num_values);

    for (uint32_t i = 0; i < num_values; ++i) {
        auto null_flag = read_pod<uint8_t>(in);
        if (null_flag == 0) {
            tuple.values[i] = Value::make_null();
            continue;
        }

        auto tid = read_pod<uint8_t>(in);
        if (tid < 1 || tid > 23) {
            return make_error(StatusCode::INTERNAL_ERROR,
                              "unknown value type index during deserialization: " +
                                  std::to_string(tid));
        }
        tuple.values[i] = read_value_payload(in, tid);
    }

    if (!in) {
        return make_error(StatusCode::IO_ERROR, "read error during tuple deserialization");
    }
    return ok(std::optional<Tuple>(std::move(tuple)));
}

// =============================================================================
// Temp-file management
// =============================================================================

std::filesystem::path ExternalSortOperator::next_temp_path() {
    return temp_dir_ / ("run_" + std::to_string(temp_counter_++) + ".tmp");
}

void ExternalSortOperator::cleanup_temp_files() {
    for (auto& r : readers_) {
        if (r.stream.is_open()) {
            r.stream.close();
        }
    }
    readers_.clear();

    for (auto& f : run_files_) {
        std::error_code ec;
        std::filesystem::remove(f, ec);
    }
    run_files_.clear();

    // Try to remove the temp directory if empty.
    std::error_code ec;
    std::filesystem::remove(temp_dir_, ec);
}

} // namespace sixseven
