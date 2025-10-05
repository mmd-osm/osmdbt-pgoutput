
#include "db.hpp"
#include "exception.hpp"

#include <cstring>
#include <iostream>
#include <string>

std::string get_db_version(pqxx::dbtransaction &txn)
{
    pqxx::result const result = txn.exec("SELECT * FROM version();");
    if (result.size() != 1) {
        throw database_error{"Expected exactly one result (version)."};
    }

    auto const &row = result[0];
    if (std::strncmp(row[0].c_str(), "PostgreSQL", 10) != 0) {
        throw database_error{"Expected version string."};
    }

    return row[0].as<std::string>();
}

int get_db_major_version(pqxx::dbtransaction &txn)
{
    pqxx::result const result = txn.exec("SHOW server_version_num;");
    if (result.size() != 1) {
        throw database_error{"Expected exactly one result (version)."};
    }

    auto const &row = result[0];

    return row[0].as<int>() / 10000;
}

void catchup_to_lsn(pqxx::dbtransaction &txn,
                    std::string const &replication_slot, std::string const &lsn)
{

    if (txn.conn().server_version() >= 110000) {
        txn.conn().prepare("advance",
                           "SELECT * FROM pg_replication_slot_advance($1,"
                           " CAST ($2 AS pg_lsn));");
    } else {
        throw database_error{"Unsupported database version, PostgreSQL 11 or newer required"};
    }

    pqxx::result const result =
        txn.exec_prepared("advance", replication_slot, lsn);

    if (result.size() != 1) {
        std::cerr << "Replication slot advance might have failed!?\n";
    }
}
