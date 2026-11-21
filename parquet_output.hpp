#pragma once

#include "identity.hpp"
#include <arrow/io/file.h>
#include <parquet/column_writer.h>
#include <parquet/file_writer.h>
#include <parquet/schema.h>

namespace pq = parquet;
using Columns = std::vector<pq::schema::NodePtr>;

inline pq::schema::NodePtr column(const char* name, pq::Type::type type,
                                  pq::ConvertedType::type annotation = pq::ConvertedType::NONE,
                                  pq::Repetition::type repetition = pq::Repetition::REQUIRED) {
    return pq::schema::PrimitiveNode::Make(name, repetition, type, annotation);
}

inline std::unique_ptr<pq::ParquetFileWriter> parquet_file(const fs::path& path, Columns columns) {
    auto result = arrow::io::FileOutputStream::Open(path.string());
    if (!result.ok()) throw std::runtime_error(result.status().ToString());
    auto schema = std::static_pointer_cast<pq::schema::GroupNode>(
        pq::schema::GroupNode::Make("schema", pq::Repetition::REQUIRED, columns));
    pq::WriterProperties::Builder options;
    options.disable_dictionary()->disable_statistics();
    return pq::ParquetFileWriter::Open(*result, std::move(schema), options.build());
}

template<class W, class T>
void put_column(pq::RowGroupWriter* group, const std::vector<T>& values) {
    auto writer = static_cast<W*>(group->NextColumn());
    writer->WriteBatch(values.size(), nullptr, nullptr, values.data());
}

inline pq::ByteArray binary(Text text) {
    if (text.size() > INT32_MAX) throw std::runtime_error("Parquet string exceeds INT32_MAX");
    return {static_cast<std::uint32_t>(text.size()), reinterpret_cast<const std::uint8_t*>(text.data())};
}

struct UserRows {
    std::unique_ptr<pq::ParquetFileWriter> file;
    std::vector<Name> names;
    std::vector<std::int32_t> days, sessions;
    std::vector<std::int64_t> events, revenue;
    std::vector<pq::ByteArray> categories;
    std::vector<std::int16_t> present;

    explicit UserRows(const fs::path& path) : file(parquet_file(path, {
        column("user_id", pq::Type::BYTE_ARRAY, pq::ConvertedType::UTF8),
        column("day", pq::Type::INT32, pq::ConvertedType::DATE),
        column("sessions", pq::Type::INT32), column("events", pq::Type::INT64),
        column("revenue_cents", pq::Type::INT64),
        column("top_category", pq::Type::BYTE_ARRAY, pq::ConvertedType::UTF8, pq::Repetition::OPTIONAL)})) {}

    void append(Name name, int day, int count, std::int64_t n, std::int64_t cents, const Text* category) {
        names.push_back(name); days.push_back(day); sessions.push_back(count);
        events.push_back(n); revenue.push_back(cents); present.push_back(category != nullptr);
        if (category) categories.push_back(binary(*category));
        if (names.size() >= 32768) flush();
    }
    void flush() {
        if (names.empty()) return;
        auto group = file->AppendRowGroup();
        std::vector<std::array<char, 9>> decoded(names.size());
        std::vector<pq::ByteArray> strings;
        strings.reserve(names.size());
        for (std::size_t i = 0; i < names.size(); ++i) strings.push_back(binary(names[i].view(decoded[i])));
        put_column<pq::ByteArrayWriter>(group, strings);
        put_column<pq::Int32Writer>(group, days);
        put_column<pq::Int32Writer>(group, sessions);
        put_column<pq::Int64Writer>(group, events);
        put_column<pq::Int64Writer>(group, revenue);
        static_cast<pq::ByteArrayWriter*>(group->NextColumn())->WriteBatch(
            names.size(), present.data(), nullptr, categories.data());
        group->Close();
        names.clear(); days.clear(); sessions.clear(); events.clear(); revenue.clear();
        present.clear(); categories.clear();
    }
    void close() { flush(); file->Close(); }
};
