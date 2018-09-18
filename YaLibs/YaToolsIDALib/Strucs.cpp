//  Copyright (C) 2017 The YaCo Authors
//
//  This program is free software: you can redistribute it and/or modify
//  it under the terms of the GNU General Public License as published by
//  the Free Software Foundation, either version 3 of the License, or
//  (at your option) any later version.
//
//  This program is distributed in the hope that it will be useful,
//  but WITHOUT ANY WARRANTY; without even the implied warranty of
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//  GNU General Public License for more details.
//
//  You should have received a copy of the GNU General Public License
//  along with this program.  If not, see <http://www.gnu.org/licenses/>.

#include "Ida.h"
#include "Strucs.hpp"

#include "Random.hpp"
#include "BinHex.hpp"
#include "Hash.hpp"
#include "YaHelpers.hpp"
#include "IModelVisitor.hpp"
#include "HVersion.hpp"
#include "Helpers.h"
#include "Logger.hpp"
#include "Yatools.hpp"

#include <unordered_map>

#define LOG(LEVEL, FMT, ...) CONCAT(YALOG_, LEVEL)("strucs", (FMT), ## __VA_ARGS__)

namespace
{
    std::string get_struc_netnode_name(const char* struc_name)
    {
        // mandatory $ prefix for user netnodes
        return std::string("$yaco_struc_") + struc_name;
    }

    std::string get_local_netnode_name(const char* local_name)
    {
        // mandatory $ prefix for user netnodes
        return std::string("$yaco_local_") + local_name;
    }

    using Reply = struct
    {
        Tag             tag;
        netnode         node;
        YaToolObjectId  id;
    };

    void get_tag_from_node(Tag& tag, const netnode& node)
    {
        node.valstr(tag.data, sizeof tag.data);
    }

    const_string_ref make_string_ref(const Tag& tag)
    {
        return {tag.data, sizeof tag.data - 1};
    }

    template<std::string(*get_name)(const char*), YaToolObjectId(*hash)(const const_string_ref&)>
    Reply hash_to_node(const char* struc_name)
    {
        const auto name = get_name(struc_name);
        netnode node;
        const auto created = node.create(name.data(), name.size());
        Tag tag;
        if(created)
        {
            uint8_t rng[sizeof tag.data >> 1];
            // generate a random value which we will assign & track
            // on our input struct
            rng::generate(&rng, sizeof rng);
            binhex(tag.data, hexchars_upper, &rng, sizeof rng);
            node.set(tag.data, sizeof tag.data - 1);
        }

        get_tag_from_node(tag, node);
        const auto id = hash(make_string_ref(tag));
        return {tag, node, id};
    }

    Reply hash_struc(const char* struc_name)
    {
        return hash_to_node<&get_struc_netnode_name, &hash::hash_struc>(struc_name);
    }

    Reply hash_local(const char* local_name)
    {
        return hash_to_node<&get_local_netnode_name, &hash::hash_local_type>(local_name);
    }

    Reply hash_with(ea_t id)
    {
        qstring qbuf;
        ya::wrap(&get_struc_name, qbuf, id);
        return hash_struc(qbuf.c_str());
    }

    void create_node_from(const std::string& name, const Tag& tag)
    {
        netnode node(name.data(), name.size(), true);
        node.set(tag.data, sizeof tag.data - 1);
    }

    Tag get_tag_from_version(const HVersion& version, bool& ok)
    {
        Tag tag;
        ok = false;
        version.walk_attributes([&](const const_string_ref& key, const const_string_ref& value)
        {
            if(key != ::make_string_ref("tag"))
                return WALK_CONTINUE;

            memcpy(tag.data, value.value, std::min(sizeof tag.data - 1, value.size));
            ok = true;
            return WALK_CONTINUE;
        });
        return tag;
    }
}

namespace strucs
{
    YaToolObjectId hash(ea_t id)
    {
        return hash_with(id).id;
    }

    Tag get_tag(ea_t id)
    {
        return hash_with(id).tag;
    }

    void rename(const char* oldname, const char* newname)
    {
        if(!oldname)
            return;

        auto node = hash_struc(oldname).node;
        const auto newnodename = get_struc_netnode_name(newname);
        node.rename(newnodename.data(), newnodename.size());
    }

    Tag remove(ea_t id)
    {
        auto r = hash_with(id);
        r.node.kill();
        return r.tag;
    }

    void set_tag_with(const char* name, const Tag& tag)
    {
        create_node_from(get_struc_netnode_name(name), tag);
    }

    void set_tag(ea_t id, const Tag& tag)
    {
        qstring qbuf;
        ya::wrap(&get_struc_name, qbuf, id);
        set_tag_with(qbuf.c_str(), tag);
    }

    void visit(IModelVisitor& v, const char* name)
    {
        const auto tag = hash_struc(name).tag;
        v.visit_attribute(make_string_ref("tag"), {tag.data, sizeof tag.data - 1});
    }

    Tag accept(const HVersion& version)
    {
        bool found = false;
        const auto tag = get_tag_from_version(version, found);
        if(found)
            set_tag_with(version.username().value, tag);
        return tag;
    }
}

namespace
{
    YaToolObjectId hash_with(tinfo_t& tif, qstring& qbuf, Tag& tag, uint32_t ord)
    {
        auto ok = tif.get_numbered_type(nullptr, ord);
        if(!ok)
            return 0;

        ok = tif.print(&qbuf);
        if(!ok)
            return 0;

        const auto r = hash_local(qbuf.c_str());
        static_assert(sizeof tag.data == sizeof r.tag.data, "tag mismatch");
        memcpy(tag.data, r.tag.data, sizeof r.tag.data);
        return r.id;
    }
}

namespace local_types
{
    bool identify(Type* type, uint32_t ord)
    {
        type->tif.clear();
        type->name.qclear();
        auto ok = type->tif.get_numbered_type(nullptr, ord);
        if(!ok)
            return false;

        ok = type->tif.print(&type->name);
        if(!ok)
            return false;

        const auto eid = get_enum(type->name.c_str());
        if(eid != BADADDR)
            return false;

        const auto sid = get_struc_id(type->name.c_str());
        if(sid == BADADDR)
            return true;

        const auto struc = get_struc(sid);
        if(!struc)
            return true;

        return struc->is_ghost();
    }

    YaToolObjectId hash(const char* name, Tag* tag)
    {
        const auto r = hash_local(name);
        if(tag)
            memcpy(tag, &r.tag, sizeof *tag);
        return r.id;
    }

    YaToolObjectId hash(uint32_t ord)
    {
        Type type;
        const auto ok = identify(&type, ord);
        if(!ok)
            return 0;

        return hash(type.name.c_str(), nullptr);
    }

    Tag get_tag(const char* name)
    {
        return hash_local(name).tag;
    }

    void rename(const char* oldname, const Tag& tag, const char* newname)
    {
        if(!oldname)
            return;

        auto node = hash_local(oldname).node;
        set_tag(oldname, tag);
        const auto newnodename = get_local_netnode_name(newname);
        node.rename(newnodename.data(), newnodename.size());
    }

    Tag remove(const char* name)
    {
        auto r = hash_local(name);
        r.node.kill();
        return r.tag;
    }

    void set_tag(const char* name, const Tag& tag)
    {
        create_node_from(get_local_netnode_name(name), tag);
    }

    void visit(IModelVisitor& v, const char* name)
    {
        const auto r = hash_local(name);
        v.visit_attribute(make_string_ref("tag"), {r.tag.data, sizeof r.tag.data - 1});
    }

    Tag accept(const HVersion& version)
    {
        bool found = false;
        const auto tag = get_tag_from_version(version, found);
        if(found)
            set_tag(make_string(version.username()).data(), tag);
        return tag;
    }
}

namespace
{
    using Tags      = std::unordered_map<std::string, std::string>;
    using Members   = std::unordered_map<YaToolObjectId, YaToolObjectId>;

    struct Filter
        : public strucs::IFilter
    {
        YaToolObjectId is_valid(const HVersion& version) override;

        Tags    strucs_;
        Tags    locals_;
        Members members_;
    };

    template<YaToolObjectId(*hasher)(const const_string_ref&)>
    YaToolObjectId check_version(Tags& tags, const HVersion& version)
    {
        const auto old = version.id();
        bool found = false;
        const auto tag_got = get_tag_from_version(version, found);
        if(!found)
            return old;

        const auto name = make_string(version.username());
        const auto it = tags.find(name);
        const auto tag = std::string{tag_got.data, sizeof tag_got.data - 1};
        if(it == tags.end())
        {
            tags.insert(std::make_pair(name, tag));
            return old;
        }

        const auto cur = hasher(::make_string_ref(it->second));
        if(old == cur)
            return old;

        return cur;
    }

    YaToolObjectId check_struc_version(Tags& tags, const HVersion& version)
    {
        return check_version<hash::hash_struc>(tags, version);
    }

    YaToolObjectId check_struc(Filter& f, const HVersion& version)
    {
        const auto old = version.id();
        const auto id = check_struc_version(f.strucs_, version);
        if(old != id)
            f.members_.emplace(old, id);
        return id;
    }

    YaToolObjectId check_member(Filter& f, const HVersion& version)
    {
        const auto old = version.id();
        const auto parent = version.parent_id();
        const auto it = f.members_.find(parent);
        if(it == f.members_.end())
            return old;

        return hash::hash_member(it->second, version.address());
    }

    YaToolObjectId check_local_type(Filter& f, const HVersion& version)
    {
        return check_version<hash::hash_local_type>(f.locals_, version);
    }
}

namespace strucs
{
    std::shared_ptr<IFilter> make_filter()
    {
        return std::make_shared<Filter>();
    }
}

YaToolObjectId Filter::is_valid(const HVersion& version)
{
    switch(version.type())
    {
        case OBJECT_TYPE_STRUCT:
            return check_struc(*this, version);

        case OBJECT_TYPE_STRUCT_MEMBER:
            return check_member(*this, version);

        case OBJECT_TYPE_LOCAL_TYPE:
            return check_local_type(*this, version);

        default:
            return version.id();
    }
}