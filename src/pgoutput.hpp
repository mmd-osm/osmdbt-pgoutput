#pragma once

#include "db.hpp"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>

namespace pgoutput {

// subset of column numbers for columns we're interested in
struct relevant_table_columns
{
    std::string relation_name; // nodes, ways or relations
    char object_type{};        // n = node, w = way, r = relation'
    int osm_object_column{};
    int changeset_column{};
    int version_column{};
    int redaction_column{};
};

using rel_id_relevant_columns = std::map<int32_t, relevant_table_columns>;

struct row_low_level_parser
{
    row_low_level_parser() = default;

    explicit row_low_level_parser(std::string_view row)
    : m_row(row)
    {
    }

    template <typename T>
    T read();

    std::string read_string();

    std::string read_string(uint32_t len);

    std::vector<std::optional<std::string>> read_tuple_data();

private:
    // pgoutput data is provided in network byte order (big endian)
    template <typename T>
    T read_network_byte_order(const char *input);

    std::string_view m_row;
    uint32_t m_offset{};
};

/**
 * @brief pgoutput binary format parser
 *
 */

struct parser
{

public:
    parser() = default;

    void set_row(std::string_view row);

    unsigned char parse_op();

    void parse_op_relation();

    std::string parse_op_insert();

    std::string parse_op_update();

private:
    row_low_level_parser m_msg;
    rel_id_relevant_columns m_relevant_columns_per_rel_id;
};

} // namespace pgoutput