#include <engrave/describe.h>

#include <algorithm>
#include <format>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "engraving/dom/property.h"
#include "engraving/style/styledef.h"

#include "engrave/src/api/pid_support.h"
#include "engrave/src/api/state.h"
#include "engrave/src/api/value_codec.h"

namespace engrave {
namespace {
using mu::engraving::Pid;
using mu::engraving::PropertyGroup;
using mu::engraving::StyleDef;

struct PidEntry {
    Pid pid;
    bool settable;
};

struct PidName {
    Pid pid;
    const char* name;
};

struct TypePids {
    const char* type;
    std::size_t first;
    std::size_t count;
};

#include "engrave/src/api/pid_support.generated.inc"

std::string_view group_name(PropertyGroup group) {
    switch (group) {
    case PropertyGroup::POSITION: return "position";
    case PropertyGroup::TEXT: return "text";
    case PropertyGroup::APPEARANCE: return "appearance";
    case PropertyGroup::NONE: return "none";
    }
    return "none";
}

template <typename Info>
void fill_value_shape(Info& info, api::P_TYPE type) {
    info.kind = api::kind_of(type);
    info.unit = api::unit_of(type);
    for (const std::string_view token : api::enum_tokens(type)) info.enum_tokens.emplace_back(token);
}

TypeInfo type_info(const TypePids& row) {
    TypeInfo info{.type = row.type};
    info.props.reserve(row.count);
    for (std::size_t i = row.first; i < row.first + row.count; ++i) {
        const PidEntry& entry = pid_support[i];
        PropInfo prop;
        prop.name = api::name_of(entry.pid);
        fill_value_shape(prop, mu::engraving::propertyType(entry.pid));
        prop.settable = entry.settable;
        prop.link = mu::engraving::propertyLink(entry.pid);
        prop.group = group_name(mu::engraving::propertyGroup(entry.pid));
        info.props.push_back(std::move(prop));
    }
    std::ranges::sort(info.props, {}, &PropInfo::name);
    return info;
}
} // namespace

std::string_view api::name_of(Pid pid) {
    for (const PidName& row : pid_names)
        if (row.pid == pid) return row.name;
    return mu::engraving::propertyName(pid);
}

Pid api::pid_of(std::string_view name) {
    for (int pid = 0; pid < int(Pid::END); ++pid)
        if (name_of(Pid(pid)) == name) return Pid(pid);
    return Pid::END;
}

bool api::has_type(std::string_view type) {
    return std::ranges::find(type_pids, type, &TypePids::type) != std::end(type_pids);
}

std::vector<Pid> api::pids_of(std::string_view type) {
    const auto row = std::ranges::find(type_pids, type, &TypePids::type);
    if (row == std::end(type_pids)) return {};
    std::vector<Pid> pids;
    pids.reserve(row->count);
    for (std::size_t i = row->first; i < row->first + row->count; ++i) pids.push_back(pid_support[i].pid);
    return pids;
}

std::optional<bool> api::settable(std::string_view type, Pid pid) {
    const auto row = std::ranges::find(type_pids, type, &TypePids::type);
    if (row == std::end(type_pids)) return std::nullopt;
    for (std::size_t i = row->first; i < row->first + row->count; ++i)
        if (pid_support[i].pid == pid) return pid_support[i].settable;
    return std::nullopt;
}

std::vector<TypeInfo> Engine::describe() const {
    impl_->state.check_thread();
    std::vector<TypeInfo> types;
    types.reserve(std::size(type_pids));
    for (const TypePids& row : type_pids) types.push_back(type_info(row));
    return types;
}

Result<TypeInfo> Engine::describe(std::string_view type) const {
    impl_->state.check_thread();
    const auto it = std::ranges::find(type_pids, type, &TypePids::type);
    if (it == std::end(type_pids))
        return std::unexpected(Error{ErrorCode::NotFound, std::format("no element type '{}'", type)});
    return type_info(*it);
}

std::vector<StyleInfo> Engine::describe_styles() const {
    impl_->state.check_thread();
    std::vector<StyleInfo> styles;
    styles.reserve(StyleDef::styleValues.size());
    for (const StyleDef::StyleValue& value : StyleDef::styleValues) {
        StyleInfo info;
        info.name = value.xmlName.ascii();
        fill_value_shape(info, value.valueType());
        info.default_value = api::to_value(value.valueType(), value.defaultValue);
        styles.push_back(std::move(info));
    }
    return styles;
}

} // namespace engrave
