
#include "config.hpp"
#include "db.hpp"
#include "io.hpp"
#include "lsn.hpp"
#include "options.hpp"
#include "pgoutput.hpp"
#include "util.hpp"

#include <osmium/util/verbose_output.hpp>

#include <algorithm>
#include <ctime>
#include <iterator>
#include <string>

namespace {

std::string_view psql_field_to_string_view(const pqxx::field& field) {
  return {field.c_str(), field.size()};
}

std::vector<char> hex2bytes(std::string_view hex)
{
    std::vector<char> bytes;

    if (hex.empty())
      return bytes;

    bytes.reserve(hex.size() / 2);

    // mapping of ASCII characters to hex values
    constexpr uint8_t hashmap[] = {
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, // 01234567
        0x08, 0x09, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // 89:;<=>?
        0x00, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x00, // @ABCDEFG
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // HIJKLMNO
    };

    for (decltype(hex.size()) pos = 0; pos < hex.size(); pos += 2) {
        uint8_t idx0 = (hex[pos + 0] & 0x1F) ^ 0x10;
        uint8_t idx1 = (hex[pos + 1] & 0x1F) ^ 0x10;
        bytes.push_back((hashmap[idx0] << 4) | hashmap[idx1]);
    };

    return bytes;
}

} // namespace

class GetLogOptions : public Options
{
public:
    GetLogOptions()
    : Options("get-log", "Write changes from replication slot to log file.")
    {}

    [[nodiscard]] bool catchup() const noexcept { return m_catchup; }

    [[nodiscard]] bool real_state() const noexcept { return m_real_state; }

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
            ("real-state,s", "Show real state (LSN and xid) instead of '0/0 0'")
            ("max-changes,m", po::value<uint32_t>(), "Maximum number of changes (default: no limit)");
        // clang-format on

        desc.add(opts_cmd);
    }

    void check_command_options(po::variables_map const &vm) override
    {
        if (vm.count("catchup")) {
            m_catchup = true;
        }
        if (vm.count("real-state")) {
            m_real_state = true;
        }
        if (vm.count("max-changes")) {
            m_max_changes = vm["max-changes"].as<uint32_t>();
        }
    }

    std::uint32_t m_max_changes = 0;
    bool m_catchup = false;
    bool m_real_state = false;
}; // class GetLogOptions

bool app(osmium::VerboseOutput &vout, Config const &config,
         GetLogOptions const &options)
{
    // All commands writing log files and/or advancing the replication slot
    // use the same pid/lock file.
    PIDFile const pid_file{config.run_dir(), "osmdbt-log"};

    vout << "Connecting to database...\n";
    pqxx::connection db{config.db_connection()};

    std::string select{"SELECT lsn, xid, encode(data, 'hex') as data FROM "
                       "pg_logical_slot_peek_binary_changes($1, NULL, "};
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
        pqxx::result const result = txn.exec_prepared(
            "peek", config.replication_slot(), config.publication());

        if (result.empty()) {
            vout << "No changes found.\n";
            vout << "Did not write log file.\n";
            txn.commit();
            vout << "Done.\n";
            return true;
        }

        vout << "There are " << result.size()
             << " entries in the replication log.\n";

        std::string data;
        data.reserve(result.size() * 50UL); // log lines should fit in 50 bytes

        bool data_in_current_transaction = false;
        bool has_actual_data = false;

        pgoutput::parser parser;

        for (auto const &row : result) {
            std::string message;

            auto hex = hex2bytes(psql_field_to_string_view(row[2]));
            std::string_view binary_string(hex.data(), hex.size());

            parser.set_row(binary_string);
            const auto op = parser.parse_op(); // read pgoutput operation

            switch (op) {

            case 'B': // begin transaction
                data_in_current_transaction = false;
                continue;

            case 'C': // commit
                message = "C";
                break;

            case 'R': // relation (pg table metadata)
            {
                parser.parse_op_relation();
                continue;
            }

            case 'I': // insert
            {
                message += parser.parse_op_insert();
                data_in_current_transaction = true;
                break;
            }

            case 'U': // update
            {
                message += parser.parse_op_update();
                data_in_current_transaction = true;
                break;
            }

            default: // skip other operations
                continue;
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
