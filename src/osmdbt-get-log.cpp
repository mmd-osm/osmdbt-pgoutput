
#include "config.hpp"
#include "db.hpp"
#include "exception.hpp"
#include "io.hpp"
#include "lsn.hpp"
#include "options.hpp"
#include "util.hpp"

#include <osmium/osm/types.hpp>
#include <osmium/util/verbose_output.hpp>

#include <algorithm>
#include <chrono>
#include <ctime>
#include <iostream>
#include <iterator>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>


class GetLogOptions : public Options
{
public:
    GetLogOptions()
    : Options("get-log", "Write changes from replication slot to log file.")
    {}

    [[nodiscard]] bool catchup() const noexcept { return m_catchup; }

    [[nodiscard]] uint32_t max_changes() const noexcept
    {
        return m_max_changes;
    }

private:
    void add_command_options(po::options_description &desc) override
    {
        po::options_description opts_cmd{"COMMAND OPTIONS"};

        // clang-format off
        opts_cmd.add_options()
            ("catchup", "Commit changes when they have been logged successfully")
            ("max-changes,m", po::value<uint32_t>(), "Maximum number of changes (default: no limit)");
        // clang-format on

        desc.add(opts_cmd);
    }

    void check_command_options(po::variables_map const &vm) override
    {
        if (vm.count("catchup")) {
            m_catchup = true;
        }
        if (vm.count("max-changes")) {
            m_max_changes = vm["max-changes"].as<uint32_t>();
        }
    }

    std::uint32_t m_max_changes = 0;
    bool m_catchup = false;
}; // class GetLogOptions


struct table_field_index {
    std::string relation_name;
    char object_type{};
    int object_index{};
    int changeset_index{};
    int version_index{};
    int redaction_index{};
};


struct message_parser_helper
{
    explicit message_parser_helper(const pqxx::binarystring &raw)
    : msg(raw.get(), raw.size())  { }

    template <typename T>
    T read() {
        auto result = read_network_byte_order<T>(msg.data() + offset);
        offset += sizeof(T);
        return result;
    }

    std::string read_string()
    {
        std::string result{msg.data() + offset};
        offset += result.size() + 1;
        return result;
    }

    std::string read_string(uint32_t len)
    {
        std::string result{msg.data() + offset, len};
        offset += len;
        return result;
    }

    std::vector< std::optional< std::string > > read_tuple_data() {

        std::vector< std::optional< std::string > > res;
        auto n_columns = read<int16_t>();

        for (int i = 0; i < n_columns; i++) {
            auto col_data_category = read<int8_t>();
            switch (col_data_category) {
                case 'n':
                case 'u':
                  res.emplace_back();
                  break;
                case 't': {
                  auto col_length = read<int32_t>();
                  res.emplace_back(read_string(col_length));
                  break;
                }
                case 'b':
                  // TODO
                  res.emplace_back();
                  break;
            }
        }

        return res;
    }


private:
    // pgoutput data is provided in network byte order (big endian)
    template <typename T>
    T read_network_byte_order(const char * input)
    {
        using T_unsigned = typename std::make_unsigned<T>::type;

        // TODO: could use bswap intrinsic instead
        T_unsigned result{};
        for (unsigned int i = 0; i < sizeof(T); i++) {
            result |= static_cast<unsigned char>(input[i]) << (8ull * (sizeof(T) - i - 1));
        }
        return static_cast<T>(result);
    }

    std::string_view msg;
    int offset{};
};

bool app(osmium::VerboseOutput &vout, Config const &config,
         GetLogOptions const &options)
{
    // All commands writing log files and/or advancing the replication slot
    // use the same pid/lock file.
    PIDFile pid_file{config.run_dir(), "osmdbt-log"};

    vout << "Connecting to database...\n";
    pqxx::connection db{config.db_connection()};

    std::string select{"SELECT * FROM pg_logical_slot_peek_binary_changes($1, NULL, "};
    if (options.max_changes() > 0) {
        vout << "Reading up to " << options.max_changes()
             << " changes (change with --max-changes)\n";
        select += std::to_string(options.max_changes());
    } else {
        vout << "Reading any number of changes (change with --max-changes)\n";
        select += "NULL";
    }
    select += ", 'proto_version', '1'";
    select += ", 'publication_names', $2);";

    db.prepare("peek", select);

    std::string lsn;

    {
        pqxx::read_transaction txn{db};
        vout << "Database version: " << get_db_version(txn) << '\n';

        vout << "Reading replication log...\n";
        pqxx::result const result =
            txn.exec_prepared("peek", config.replication_slot(), config.publication());

        if (result.empty()) {
            vout << "No changes found.\n";
            vout << "Did not write log file.\n";
            txn.commit();
            vout << "Done.\n";
            return true;
        }

        vout << "There are " << result.size()
             << " entries in the replication log.\n";

        std::map<int32_t, table_field_index> field_idx_by_relation_id;

        std::string data;
        data.reserve(result.size() * 50); // log lines should fit in 50 bytes

        bool data_in_current_transaction = false;
        bool has_actual_data = false;
        for (auto const &row : result) {
            pqxx::binarystring binary_string{row[2]};
            message_parser_helper msg{binary_string};

            const char op = msg.read<uint8_t>();

            std::string message{};

            // Reference for pgoutput decoding:
            // https://www.postgresql.org/docs/current/protocol-logicalrep-message-formats.html

            {
                  switch (op) {

                  case 'B': // begin
                      data_in_current_transaction = false;
                      continue;

                  case 'C': // commit
                      message = "C";
                      break;

                  case 'R': // relation
                  {
                      auto id = msg.read<int32_t>();
                      auto ns = msg.read_string();
                      auto relation_name = msg.read_string();
                      /* auto replica_identity = */ msg.read<int8_t>();
                      auto number_of_columns = msg.read<uint16_t>();

                      table_field_index idx;
                      std::string object_id_field;

                      idx.relation_name = relation_name;

                      if (relation_name == "nodes") {
                          idx.object_type = 'n';
                          object_id_field = "node_id";

                      } else if (relation_name == "ways") {
                          idx.object_type = 'w';
                          object_id_field = "way_id";

                      } else if (relation_name == "relations") {
                          idx.object_type = 'r';
                          object_id_field = "relation_id";
                      } else {
                          continue;
                      }

                      int found_columns = 0;

                      for (int col = 0; col < number_of_columns; col++) {
                          /* auto flags = */ msg.read<int8_t>();
                          auto column_name = msg.read_string();
                          /* auto column_type = */ msg.read<int32_t>();
                          /* auto type_modifier = */ msg.read<int32_t>();

                          if (column_name == object_id_field) {
                              idx.object_index = col;
                              found_columns++;
                          } else if (column_name == "changeset_id") {
                              idx.changeset_index = col;
                              found_columns++;
                          } else if (column_name == "version") {
                              idx.version_index = col;
                              found_columns++;
                          } else if (column_name == "redaction_id") {
                              idx.redaction_index = col;
                              found_columns++;
                          }
                      }

                      if (found_columns != 4) {
                          vout << "Missing column in relation " << relation_name
                               << "\n";
                          return false;
                      }

                      field_idx_by_relation_id[id] = idx;
                      continue;
                  }

                  case 'I': // insert
                  {
                      auto relation_id = msg.read<int32_t>();
                      /* auto new_tuple_byte = */ msg.read<uint8_t>();
                      auto new_tuple = msg.read_tuple_data();
                      auto field_idx = field_idx_by_relation_id.at(relation_id);

                      message += "N ";
                      message += field_idx.object_type;
                      message +=
                          new_tuple[field_idx.object_index].value_or("XXX");
                      message += " v";
                      message +=
                          new_tuple[field_idx.version_index].value_or("XXX");
                      message += " c";
                      message +=
                          new_tuple[field_idx.changeset_index].value_or("XXX");

                      data_in_current_transaction = true;
                      break;
                  }

                  case 'U': // update
                  {
                      auto relation_id = msg.read<int32_t>();
                      auto tuple_byte = msg.read<uint8_t>();
                      // consume key or old tuple data, we only care about the new tuple
                      if (tuple_byte == 'K' || tuple_byte == 'O') {
                          msg.read_tuple_data();
                          tuple_byte = msg.read<uint8_t>();
                      }

                      if (tuple_byte != 'N') {
                          vout << "Update: expected N tuple byte\n";
                          return false;
                      }

                      auto new_tuple = msg.read_tuple_data();
                      auto field_idx = field_idx_by_relation_id.at(relation_id);

                      if (!new_tuple[field_idx.redaction_index].has_value()) {
                          message +=
                              "UPDATE with redaction_id set to NULL for " +
                              field_idx.relation_name;
                      }

                      message += "R ";
                      message += field_idx.object_type;
                      message +=
                          new_tuple[field_idx.object_index].value_or("XXX");
                      message += " v";
                      message +=
                          new_tuple[field_idx.version_index].value_or("XXX");
                      message += " c";
                      message +=
                          new_tuple[field_idx.changeset_index].value_or("XXX");
                      message += " ";
                      message +=
                          new_tuple[field_idx.redaction_index].value_or("");

                      data_in_current_transaction = true;
                      break;
                  }

                  default:
                      continue;
                  }
            }

            if (data_in_current_transaction) {
                data.append(row[0].c_str());
                data += ' ';
                data.append(row[1].c_str());
                data += ' ';
                data.append(message);
                data += '\n';
            }

            if (message[0] == 'C') {
                lsn = row[0].c_str();
                data_in_current_transaction = false;
            } else if (message[0] == 'N') {
                has_actual_data = true;
            }
        }

        vout << "LSN is " << lsn << '\n';

        if (has_actual_data) {
            std::string lsn_dash{"lsn-"};
            std::transform(lsn.cbegin(), lsn.cend(),
                           std::back_inserter(lsn_dash),
                           [](char c) { return c == '/' ? '-' : c; });

            std::string const file_name = create_replication_log_name(lsn_dash);
            vout << "Writing log to '" << config.log_dir() << file_name
                 << "'...\n";

            write_data_to_file(data, config.log_dir(), file_name);
            vout << "Wrote and synced log.\n";
        } else {
            vout << "No actual changes found.\n";
            vout << "Did not write log file.\n";
        }
    }

    if (options.catchup()) {
        vout << "Catching up to " << lsn << "...\n";
        pqxx::work txn{db};
        catchup_to_lsn(txn, config.replication_slot(), lsn_type{lsn}.str());
        txn.commit();
    } else {
        vout << "Not catching up (use --catchup if you want this).\n";
    }

    vout << "Done.\n";

    return true;
}

int main(int argc, char *argv[])
{
    GetLogOptions options;
    return app_wrapper(options, argc, argv);
}
