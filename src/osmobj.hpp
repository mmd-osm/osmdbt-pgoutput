#pragma once

#include <osmium/builder/osm_object_builder.hpp>
#include <osmium/index/nwr_array.hpp>
#include <osmium/osm/types.hpp>

#include <cassert>
#include <stdexcept>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

struct userinfo
{
    osmium::user_id_type id = 0;
    std::string username;
};

using changeset_user_lookup =
    std::unordered_map<osmium::changeset_id_type, userinfo>;

class osmobj
{
public:
    explicit osmobj(std::string const &obj, std::string const &version,
                    std::string const &changeset,
                    changeset_user_lookup *cucache = nullptr);

    [[nodiscard]] osmium::item_type type() const noexcept { return m_type; }

    [[nodiscard]] osmium::object_id_type id() const noexcept { return m_id; }

    [[nodiscard]] osmium::object_version_type version() const noexcept
    {
        return m_version;
    }

    [[nodiscard]] osmium::changeset_id_type cid() const noexcept
    {
        return m_cid;
    }

    using osmobj_tuple = std::tuple<unsigned int, osmium::object_id_type,
                                    osmium::object_version_type>;

    friend bool operator<(osmobj const &lhs, osmobj const &rhs) noexcept
    {
        return osmobj_tuple{osmium::item_type_to_nwr_index(lhs.type()),
                            lhs.id(), lhs.version()} <
               osmobj_tuple{osmium::item_type_to_nwr_index(rhs.type()),
                            rhs.id(), rhs.version()};
    }

    friend bool operator>(osmobj const &lhs, osmobj const &rhs) noexcept
    {
        return osmobj_tuple{osmium::item_type_to_nwr_index(lhs.type()),
                            lhs.id(), lhs.version()} >
               osmobj_tuple{osmium::item_type_to_nwr_index(rhs.type()),
                            rhs.id(), rhs.version()};
    }

    friend bool operator<=(osmobj const &lhs, osmobj const &rhs) noexcept
    {
        return !(lhs > rhs);
    }

    friend bool operator>=(osmobj const &lhs, osmobj const &rhs) noexcept
    {
        return !(lhs < rhs);
    }

private:
    osmium::item_type m_type;
    osmium::object_id_type m_id;
    osmium::object_version_type m_version;
    osmium::changeset_id_type m_cid;

}; // class osmobj

class osmobjects
{
public:
    [[nodiscard]] std::vector<osmobj> const &nodes() const noexcept
    {
        return m_objects(osmium::item_type::node);
    }

    [[nodiscard]] std::vector<osmobj> const &ways() const noexcept
    {
        return m_objects(osmium::item_type::way);
    }

    [[nodiscard]] std::vector<osmobj> const &relations() const noexcept
    {
        return m_objects(osmium::item_type::relation);
    }

    [[nodiscard]] std::size_t size() const noexcept
    {
        return m_objects(osmium::item_type::node).size() +
               m_objects(osmium::item_type::way).size() +
               m_objects(osmium::item_type::relation).size();
    }

    [[nodiscard]] bool empty() const noexcept
    {
        return m_objects(osmium::item_type::node).empty() &&
               m_objects(osmium::item_type::way).empty() &&
               m_objects(osmium::item_type::relation).empty();
    }

    void add(std::string const &type_id, std::string const &version,
             std::string const &changeset, changeset_user_lookup *cucache)
    {
        osmobj const obj{type_id, version, changeset, cucache};
        m_objects(obj.type()).push_back(obj);
    }

    void sort();

private:
    osmium::nwr_array<std::vector<osmobj>> m_objects;

}; // class osmobjects

void read_log(osmobjects &objects_todo, std::string const &dir_name,
              std::string const &file_name,
              changeset_user_lookup *cucache = nullptr);
