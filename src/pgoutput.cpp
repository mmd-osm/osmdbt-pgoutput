#include "pgoutput.hpp"

// Reference documentation for pgoutput decoding:
// https://www.postgresql.org/docs/current/protocol-logicalrep-message-formats.html

namespace pgoutput {

template <typename T>
T row_low_level_parser::read()
{
    auto result = read_network_byte_order<T>(m_row.data() + m_offset);
    m_offset += sizeof(T);
    return result;
}

std::string row_low_level_parser::read_string()
{
    std::string result{m_row.data() + m_offset};
    m_offset += result.size() + 1;
    return result;
}

std::string row_low_level_parser::read_string(uint32_t len)
{
    std::string result{m_row.data() + m_offset, len};
    m_offset += len;
    return result;
}

std::vector<std::optional<std::string>> row_low_level_parser::read_tuple_data()
{
    std::vector<std::optional<std::string>> result;
    auto n_columns = read<int16_t>();

    for (int i = 0; i < n_columns; i++) {
        auto col_data_category = read<int8_t>();
        switch (col_data_category) {
        case 'n': // null value column
        case 'u':
            result.emplace_back();
            break;
        case 't': {
            // text column
            auto col_length = read<int32_t>();
            result.emplace_back(read_string(col_length));
            break;
        }
        case 'b':
            // ignore binary column for now
            result.emplace_back();
            break;
        default:
            // ignore unknown column category
            result.emplace_back();
            break;
        }
    }

    return result;
}

// pgoutput data is provided in network byte order (big endian)
template <typename T>
T row_low_level_parser::read_network_byte_order(const char *input)
{
    using T_unsigned = typename std::make_unsigned<T>::type;

    // TODO: could use bswap intrinsic instead
    T_unsigned result{};
    for (unsigned int i = 0; i < sizeof(T); i++) {
        result += static_cast<unsigned char>(input[i])
                  << (8ULL * (sizeof(T) - i - 1));
    }
    return static_cast<T>(result);
}

void parser::set_row(const pqxx::binarystring &row)
{
    m_msg = row_low_level_parser(row);
}

unsigned char parser::parse_op() { return m_msg.read<uint8_t>(); }

void parser::parse_op_relation()
{
    relevant_table_columns cols;
    std::string object_id_field;

    auto relation_id = m_msg.read<int32_t>();
    auto ns = m_msg.read_string();
    auto relation_name = m_msg.read_string();
    /* auto replica_identity = */ m_msg.read<int8_t>();
    auto number_of_columns = m_msg.read<uint16_t>();

    cols.relation_name = relation_name;

    if (relation_name == "nodes") {
        cols.object_type = 'n';
        object_id_field = "node_id";

    } else if (relation_name == "ways") {
        cols.object_type = 'w';
        object_id_field = "way_id";

    } else if (relation_name == "relations") {
        cols.object_type = 'r';
        object_id_field = "relation_id";
    } else {
        throw std::runtime_error(
            "pgoutput provided unexpected relation metadata for " +
            relation_name + " (relation_id: " + std::to_string(relation_id) +
            ")");
    }

    int found_columns = 0;

    for (int col = 0; col < number_of_columns; col++) {
        /* auto flags = */ m_msg.read<int8_t>();
        auto column_name = m_msg.read_string();
        /* auto column_type = */ m_msg.read<int32_t>();
        /* auto type_modifier = */ m_msg.read<int32_t>();

        if (column_name == object_id_field) {
            cols.osm_object_column = col;
            found_columns++;
        } else if (column_name == "changeset_id") {
            cols.changeset_column = col;
            found_columns++;
        } else if (column_name == "version") {
            cols.version_column = col;
            found_columns++;
        } else if (column_name == "redaction_id") {
            cols.redaction_column = col;
            found_columns++;
        }
    }

    if (found_columns != 4) {
        throw std::runtime_error("Missing column in relation " + relation_name);
    }

    m_relevant_columns_per_rel_id[relation_id] = cols;
}

std::string parser::parse_op_insert()
{
    std::string result;
    auto relation_id = m_msg.read<int32_t>();
    /* auto new_tuple_byte = */ m_msg.read<uint8_t>();
    auto new_tuple = m_msg.read_tuple_data();

    if (m_relevant_columns_per_rel_id.find(relation_id) ==
        m_relevant_columns_per_rel_id.end()) {
        throw std::runtime_error("Missing metadata for relation id " +
                                 std::to_string(relation_id));
    }

    auto columns = m_relevant_columns_per_rel_id[relation_id];
    result += "N ";
    result += columns.object_type;
    result += *new_tuple[columns.osm_object_column];
    result += " v";
    result += *new_tuple[columns.version_column];
    result += " c";
    result += *new_tuple[columns.changeset_column];
    return result;
}

std::string parser::parse_op_update()
{
    std::string result;
    auto relation_id = m_msg.read<int32_t>();
    auto tuple_byte = m_msg.read<uint8_t>();
    // skip key field and old tuple, we only care about the new tuple
    if (tuple_byte == 'K' || tuple_byte == 'O') {
        m_msg.read_tuple_data();
        tuple_byte = m_msg.read<uint8_t>();
    }

    if (tuple_byte != 'N') {
        throw std::runtime_error("Update: expected N tuple byte");
    }

    auto new_tuple = m_msg.read_tuple_data();

    if (m_relevant_columns_per_rel_id.find(relation_id) ==
        m_relevant_columns_per_rel_id.end()) {
        throw std::runtime_error("Missing metadata for relation id " +
                                 std::to_string(relation_id));
    }

    auto columns = m_relevant_columns_per_rel_id[relation_id];

    if (!new_tuple[columns.redaction_column].has_value()) {
        result += "UPDATE with redaction_id set to NULL for " +
                   columns.relation_name + " ";
    } else {
        result += "R ";
    }

    result += columns.object_type;
    result += *new_tuple[columns.osm_object_column];
    result += " v";
    result += *new_tuple[columns.version_column];
    result += " c";
    result += *new_tuple[columns.changeset_column];
    result += " ";
    result += new_tuple[columns.redaction_column].value_or("");
    return result;
}

} // namespace pgoutput