// Copyright 2010-2014 RethinkDB, all rights reserved.
#include "rdb_protocol/datum.hpp"

#include <float.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>

#include <algorithm>
#include <iterator>

#include "errors.hpp"
#include <boost/detail/endian.hpp>

#include "containers/archive/stl_types.hpp"
#include "containers/scoped.hpp"
#include "rdb_protocol/env.hpp"
#include "rdb_protocol/error.hpp"
#include "rdb_protocol/pseudo_binary.hpp"
#include "rdb_protocol/pseudo_geometry.hpp"
#include "rdb_protocol/pseudo_literal.hpp"
#include "rdb_protocol/pseudo_time.hpp"
#include "rdb_protocol/serialize_datum.hpp"
#include "rdb_protocol/shards.hpp"
#include "parsing/utf8.hpp"
#include "stl_utils.hpp"

namespace ql {

const size_t tag_size = 8;

const std::set<std::string> datum_t::_allowed_pts = std::set<std::string>();

const datum_string_t datum_t::reql_type_string("$reql_type$");

const datum_string_t errors_field("errors");
const datum_string_t first_error_field("first_error");
const datum_string_t warnings_field("warnings");
const datum_string_t data_field("data");

datum_t::data_wrapper_t::data_wrapper_t(const datum_t::data_wrapper_t &copyee) {
    assign_copy(copyee);
}

datum_t::data_wrapper_t::data_wrapper_t(datum_t::data_wrapper_t &&movee) noexcept {
    assign_move(std::move(movee));
}

datum_t::data_wrapper_t &datum_t::data_wrapper_t::operator=(
        const datum_t::data_wrapper_t &copyee) {
    // Ignore self-assignment
    if (&copyee == this) {
        return *this;
    }

    // Move our data out of the way to keep counted values alive, in case
    // copyee is actually pointing to a nested element of ourselves.
    UNUSED data_wrapper_t this_copy(std::move(*this));

    // Destruct our own references
    destruct();

    // Assign new values
    assign_copy(copyee);

    return *this;
}

datum_t::data_wrapper_t::data_wrapper_t() :
    internal_type(internal_type_t::UNINITIALIZED) { }

datum_t::data_wrapper_t::data_wrapper_t(datum_t::construct_null_t) :
    internal_type(internal_type_t::R_NULL) { }

datum_t::data_wrapper_t::data_wrapper_t(datum_t::construct_boolean_t, bool _bool) :
    r_bool(_bool), internal_type(internal_type_t::R_BOOL) { }

datum_t::data_wrapper_t::data_wrapper_t(datum_t::construct_binary_t,
                                        datum_string_t _data) :
    r_str(std::move(_data)), internal_type(internal_type_t::R_BINARY) { }

datum_t::data_wrapper_t::data_wrapper_t(double num) :
    r_num(num), internal_type(internal_type_t::R_NUM) { }

datum_t::data_wrapper_t::data_wrapper_t(datum_string_t str) :
    r_str(std::move(str)), internal_type(internal_type_t::R_STR) { }

datum_t::data_wrapper_t::data_wrapper_t(const char *cstr) :
    r_str(cstr), internal_type(internal_type_t::R_STR) { }

datum_t::data_wrapper_t::data_wrapper_t(std::vector<datum_t> &&array) :
    r_array(new countable_wrapper_t<std::vector<datum_t> >(std::move(array))),
    internal_type(internal_type_t::R_ARRAY) { }

datum_t::data_wrapper_t::data_wrapper_t(
        std::vector<std::pair<datum_string_t, datum_t> > &&object) :
    r_object(new countable_wrapper_t<std::vector<std::pair<datum_string_t, datum_t> > >(
        std::move(object))),
    internal_type(internal_type_t::R_OBJECT) {

#ifndef NDEBUG
    auto key_cmp = [](const std::pair<datum_string_t, datum_t> &p1,
                      const std::pair<datum_string_t, datum_t> &p2) -> bool {
        return p1.first < p2.first;
    };
    rassert(std::is_sorted(r_object->begin(), r_object->end(), key_cmp));
#endif
}

datum_t::data_wrapper_t::data_wrapper_t(type_t type, shared_buf_ref_t<char> &&_buf_ref) {
    switch (type) {
    case R_BINARY: {
        internal_type = internal_type_t::R_BINARY;
        new(&r_str) datum_string_t(std::move(_buf_ref));
    } break;
    case R_ARRAY: {
        internal_type = internal_type_t::BUF_R_ARRAY;
        new(&buf_ref) shared_buf_ref_t<char>(std::move(_buf_ref));
    } break;
    case R_OBJECT: {
        internal_type = internal_type_t::BUF_R_OBJECT;
        new(&buf_ref) shared_buf_ref_t<char>(std::move(_buf_ref));
    } break;
    case R_STR: {
        internal_type = internal_type_t::R_STR;
        new(&r_str) datum_string_t(std::move(_buf_ref));
    } break;
    case UNINITIALIZED: // fallthru
    case R_BOOL: // fallthru
    case R_NULL: // fallthru
    case R_NUM: // fallthru
    default:
        unreachable();
    }
}

datum_t::data_wrapper_t::~data_wrapper_t() {
    destruct();
}

datum_t::type_t datum_t::data_wrapper_t::get_type() const {
    switch (internal_type) {
    case internal_type_t::UNINITIALIZED:
        return type_t::UNINITIALIZED;
    case internal_type_t::R_ARRAY:
        return type_t::R_ARRAY;
    case internal_type_t::R_BINARY:
        return type_t::R_BINARY;
    case internal_type_t::R_BOOL:
        return type_t::R_BOOL;
    case internal_type_t::R_NULL:
        return type_t::R_NULL;
    case internal_type_t::R_NUM:
        return type_t::R_NUM;
    case internal_type_t::R_OBJECT:
        return type_t::R_OBJECT;
    case internal_type_t::R_STR:
        return type_t::R_STR;
    case internal_type_t::BUF_R_ARRAY:
        return type_t::R_ARRAY;
    case internal_type_t::BUF_R_OBJECT:
        return type_t::R_OBJECT;
    default:
        unreachable();
    }
}
datum_t::internal_type_t datum_t::data_wrapper_t::get_internal_type() const {
    return internal_type;
}

void datum_t::data_wrapper_t::destruct() {
    switch (internal_type) {
    case internal_type_t::UNINITIALIZED: // fallthru
    case internal_type_t::R_NULL: // fallthru
    case internal_type_t::R_BOOL: // fallthru
    case internal_type_t::R_NUM: break;
    case internal_type_t::R_BINARY: // fallthru
    case internal_type_t::R_STR: {
        r_str.~datum_string_t();
    } break;
    case internal_type_t::R_ARRAY: {
        r_array.~counted_t<countable_wrapper_t<std::vector<datum_t> > >();
    } break;
    case internal_type_t::R_OBJECT: {
        r_object.~counted_t<countable_wrapper_t<std::vector<std::pair<datum_string_t, datum_t> > > >();
    } break;
    case internal_type_t::BUF_R_ARRAY: // fallthru
    case internal_type_t::BUF_R_OBJECT: {
        buf_ref.~shared_buf_ref_t<char>();
    } break;
    default: unreachable();
    }
}

void datum_t::data_wrapper_t::assign_copy(const datum_t::data_wrapper_t &copyee) {
    internal_type = copyee.internal_type;
    switch (internal_type) {
    case internal_type_t::UNINITIALIZED: // fallthru
    case internal_type_t::R_NULL: break;
    case internal_type_t::R_BOOL: {
        r_bool = copyee.r_bool;
    } break;
    case internal_type_t::R_NUM: {
        r_num = copyee.r_num;
    } break;
    case internal_type_t::R_BINARY: // fallthru
    case internal_type_t::R_STR: {
        new(&r_str) datum_string_t(copyee.r_str);
    } break;
    case internal_type_t::R_ARRAY: {
        new(&r_array) counted_t<countable_wrapper_t<std::vector<datum_t> > >(copyee.r_array);
    } break;
    case internal_type_t::R_OBJECT: {
        new(&r_object) counted_t<countable_wrapper_t<std::vector<std::pair<datum_string_t, datum_t> > > >(
            copyee.r_object);
    } break;
    case internal_type_t::BUF_R_ARRAY: // fallthru
    case internal_type_t::BUF_R_OBJECT: {
        new(&buf_ref) shared_buf_ref_t<char>(copyee.buf_ref);
    } break;
    default: unreachable();
    }
}

void datum_t::data_wrapper_t::assign_move(datum_t::data_wrapper_t &&movee) noexcept {
    internal_type = movee.internal_type;
    switch (internal_type) {
    case internal_type_t::UNINITIALIZED: // fallthru
    case internal_type_t::R_NULL: break;
    case internal_type_t::R_BOOL: {
        r_bool = movee.r_bool;
    } break;
    case internal_type_t::R_NUM: {
        r_num = movee.r_num;
    } break;
    case internal_type_t::R_BINARY: // fallthru
    case internal_type_t::R_STR: {
        new(&r_str) datum_string_t(std::move(movee.r_str));
    } break;
    case internal_type_t::R_ARRAY: {
        new(&r_array) counted_t<countable_wrapper_t<std::vector<datum_t> > >(
            std::move(movee.r_array));
    } break;
    case internal_type_t::R_OBJECT: {
        new(&r_object) counted_t<countable_wrapper_t<std::vector<std::pair<datum_string_t, datum_t> > > >(
            std::move(movee.r_object));
    } break;
    case internal_type_t::BUF_R_ARRAY: // fallthru
    case internal_type_t::BUF_R_OBJECT: {
        new(&buf_ref) shared_buf_ref_t<char>(std::move(movee.buf_ref));
    } break;
    default: unreachable();
    }
}

datum_t::datum_t() : data() { }

datum_t::datum_t(type_t type, shared_buf_ref_t<char> &&buf_ref)
    : data(type, std::move(buf_ref)) { }

datum_t::datum_t(datum_t::construct_null_t dummy) : data(dummy) { }

datum_t::datum_t(construct_boolean_t dummy, bool _bool) : data(dummy, _bool) { }

datum_t::datum_t(construct_binary_t dummy, datum_string_t _data)
    : data(dummy, std::move(_data)) { }

datum_t::datum_t(double _num) : data(_num) {
    rcheck(risfinite(data.r_num), base_exc_t::GENERIC,
           strprintf("Non-finite number: %" PR_RECONSTRUCTABLE_DOUBLE, data.r_num));
}

datum_t::datum_t(datum_string_t _str) : data(std::move(_str)) {
    check_str_validity(data.r_str);
}

datum_t::datum_t(const char *cstr) : data(cstr) { }

datum_t::datum_t(std::vector<datum_t> &&_array,
                 const configured_limits_t &limits)
    : data(std::move(_array)) {
    rcheck_array_size(*data.r_array, limits, base_exc_t::GENERIC);
}

datum_t::datum_t(std::vector<datum_t> &&_array,
                 no_array_size_limit_check_t) : data(std::move(_array)) { }

datum_t::datum_t(std::map<datum_string_t, datum_t> &&_object,
                 const std::set<std::string> &allowed_pts)
    : data(to_sorted_vec(std::move(_object))) {
    maybe_sanitize_ptype(allowed_pts);
}

datum_t::datum_t(std::vector<std::pair<datum_string_t, datum_t> > &&_object,
                 const std::set<std::string> &allowed_pts)
    : data(std::move(_object)) {
    maybe_sanitize_ptype(allowed_pts);
}

datum_t::datum_t(std::map<datum_string_t, datum_t> &&_object,
                 no_sanitize_ptype_t)
    : data(to_sorted_vec(std::move(_object))) { }

std::vector<std::pair<datum_string_t, datum_t> > datum_t::to_sorted_vec(
        std::map<datum_string_t, datum_t> &&map) {
    std::vector<std::pair<datum_string_t, datum_t> > sorted_vec;
    sorted_vec.reserve(map.size());
    for (auto it = map.begin(); it != map.end(); ++it) {
        sorted_vec.push_back(std::make_pair(std::move(it->first), std::move(it->second)));
    }
    return sorted_vec;
}

datum_t to_datum_for_client_serialization(grouped_data_t &&gd,
                                          reql_version_t reql_version,
                                          const configured_limits_t &limits) {
    std::map<datum_string_t, datum_t> map;
    map[datum_t::reql_type_string] =
        datum_t("GROUPED_DATA");

    {
        datum_array_builder_t arr(limits);
        arr.reserve(gd.size());
        iterate_ordered_by_version(
                reql_version,
                gd,
                [&arr, &limits](const datum_t &key,
                                datum_t &value) {
                    arr.add(datum_t(
                            std::vector<datum_t>{
                                key, std::move(value) },
                            limits));
                });
        map[data_field] = std::move(arr).to_datum();
    }

    // We don't sanitize the ptype because this is a fake ptype that should only
    // be used for serialization.
    // TODO(2015-01): This is a bad thing.
    return datum_t(std::move(map), datum_t::no_sanitize_ptype_t());
}

datum_t::~datum_t() {
}

bool datum_t::has() const {
    return data.get_type() != UNINITIALIZED;
}

void datum_t::reset() {
    data = data_wrapper_t();
}

datum_t datum_t::empty_array() {
    return datum_t(std::vector<datum_t>(),
                   no_array_size_limit_check_t());
}

datum_t datum_t::empty_object() {
    return datum_t(std::map<datum_string_t, datum_t>());
}

datum_t datum_t::null() {
    return datum_t(construct_null_t());
}

datum_t datum_t::boolean(bool value) {
    return datum_t(construct_boolean_t(), value);
}

datum_t datum_t::binary(const datum_string_t &_data) {
    return datum_t(construct_binary_t(), _data);
}

datum_t datum_t::binary(datum_string_t &&_data) {
    return datum_t(construct_binary_t(), std::move(_data));
}

// two versions of these, because std::string is not necessarily null
// terminated.
inline void fail_if_invalid(reql_version_t reql_version, const std::string &string)
{
    switch (reql_version) {
        case reql_version_t::v1_13:
        case reql_version_t::v1_14: // v1_15 is the same as v1_14
            break;
        case reql_version_t::v1_16_is_latest:
            utf8::reason_t reason;
            if (!utf8::is_valid(string, &reason)) {
                int truncation_length = std::min(reason.position, 20ul);
                rfail_datum(base_exc_t::GENERIC,
                            "String `%.*s` (truncated) is not a UTF-8 string; "
                            "%s at position %zu.",
                            truncation_length, string.c_str(), reason.explanation,
                            reason.position);
            }
            break;
        default:
            unreachable();
    }
}

inline void fail_if_invalid(reql_version_t reql_version, const char *string)
{
    switch (reql_version) {
        case reql_version_t::v1_13:
        case reql_version_t::v1_14: // v1_15 is the same as v1_14
            break;
        case reql_version_t::v1_16_is_latest:
            utf8::reason_t reason;
            if (!utf8::is_valid(string, &reason)) {
                int truncation_length = std::min(reason.position, 20ul);
                rfail_datum(base_exc_t::GENERIC,
                            "String `%.*s` (truncated) is not a UTF-8 string; "
                            "%s at position %zu.",
                            truncation_length, string, reason.explanation,
                            reason.position);
            }
            break;
        default:
            unreachable();
    }
}

datum_t to_datum(cJSON *json, const configured_limits_t &limits,
                 reql_version_t reql_version) {
    switch (json->type) {
    case cJSON_False: {
        return datum_t::boolean(false);
    } break;
    case cJSON_True: {
        return datum_t::boolean(true);
    } break;
    case cJSON_NULL: {
        return datum_t::null();
    } break;
    case cJSON_Number: {
        return datum_t(json->valuedouble);
    } break;
    case cJSON_String: {
        fail_if_invalid(reql_version, json->valuestring);
        return datum_t(json->valuestring);
    } break;
    case cJSON_Array: {
        std::vector<datum_t> array;
        json_array_iterator_t it(json);
        while (cJSON *item = it.next()) {
            array.push_back(to_datum(item, limits, reql_version));
        }
        return datum_t(std::move(array), limits);
    } break;
    case cJSON_Object: {
        datum_object_builder_t builder;
        json_object_iterator_t it(json);
        while (cJSON *item = it.next()) {
            fail_if_invalid(reql_version, item->string);
            bool dup = builder.add(item->string, to_datum(item, limits, reql_version));
            rcheck_datum(!dup, base_exc_t::GENERIC,
                         strprintf("Duplicate key `%s` in JSON.", item->string));
        }
        const std::set<std::string> pts = { pseudo::literal_string };
        return std::move(builder).to_datum(pts);
    } break;
    default: unreachable();
    }
}


void check_str_validity(const char *bytes, size_t count) {
    const char *pos = static_cast<const char *>(memchr(bytes, 0, count));
    rcheck_datum(pos == NULL,
                 base_exc_t::GENERIC,
                 // We truncate because lots of other places can call `c_str` on the
                 // error message.
                 strprintf("String `%.20s` (truncated) contains NULL byte at offset %zu.",
                           bytes, pos - bytes));
}

void datum_t::check_str_validity(const datum_string_t &str) {
    ::ql::check_str_validity(str.data(), str.size());
}

const shared_buf_ref_t<char> *datum_t::get_buf_ref() const {
    if (data.get_internal_type() == internal_type_t::BUF_R_ARRAY
        || data.get_internal_type() == internal_type_t::BUF_R_OBJECT) {
        return &data.buf_ref;
    } else {
        return NULL;
    }
}

datum_t::type_t datum_t::get_type() const { return data.get_type(); }

bool datum_t::is_ptype() const {
    return get_type() == R_BINARY ||
        (get_type() == R_OBJECT && get_field(reql_type_string, NOTHROW).has());
}

bool datum_t::is_ptype(const std::string &reql_type) const {
    return (reql_type == "") ? is_ptype() : is_ptype() && get_reql_type() == reql_type;
}

std::string datum_t::get_reql_type() const {
    r_sanity_check(is_ptype());
    if (get_type() == R_BINARY) {
        return "BINARY";
    }

    datum_t maybe_reql_type = get_field(reql_type_string, NOTHROW);
    r_sanity_check(maybe_reql_type.has());
    rcheck(maybe_reql_type.get_type() == R_STR,
           base_exc_t::GENERIC,
           strprintf("Error: Field `%s` must be a string (got `%s` of type %s):\n%s",
                     reql_type_string.to_std().c_str(),
                     maybe_reql_type.trunc_print().c_str(),
                     maybe_reql_type.get_type_name().c_str(),
                     trunc_print().c_str()));
    return maybe_reql_type.as_str().to_std();
}

std::string raw_type_name(datum_t::type_t type) {
    switch (type) {
    case datum_t::R_NULL:   return "NULL";
    case datum_t::R_BINARY: return std::string("PTYPE<") + pseudo::binary_string + ">";
    case datum_t::R_BOOL:   return "BOOL";
    case datum_t::R_NUM:    return "NUMBER";
    case datum_t::R_STR:    return "STRING";
    case datum_t::R_ARRAY:  return "ARRAY";
    case datum_t::R_OBJECT: return "OBJECT";
    case datum_t::UNINITIALIZED: // fallthru
    default: unreachable();
    }
}

std::string datum_t::get_type_name() const {
    if (is_ptype()) {
        return "PTYPE<" + get_reql_type() + ">";
    } else {
        return raw_type_name(get_type());
    }
}

std::string datum_t::print() const {
    return has() ? as_json().Print() : "UNINITIALIZED";
}

std::string datum_t::trunc_print() const {
    std::string s = print();
    if (s.size() > trunc_len) {
        s.erase(s.begin() + (trunc_len - 3), s.end());
        s += "...";
    }
    return s;
}

void datum_t::pt_to_str_key(std::string *str_out) const {
    r_sanity_check(is_ptype());
    if (get_reql_type() == pseudo::time_string) {
        pseudo::time_to_str_key(*this, str_out);
    } else if (get_reql_type() == pseudo::geometry_string) {
        rfail(base_exc_t::GENERIC,
              "Cannot use a geometry value as a key value in a primary or "
              "non-geospatial secondary index.");
    } else {
        rfail(base_exc_t::GENERIC,
              "Cannot use pseudotype %s as a primary or secondary key value .",
              get_type_name().c_str());
    }
}

void datum_t::num_to_str_key(std::string *str_out) const {
    r_sanity_check(get_type() == R_NUM);
    str_out->append("N");
    union {
        double d;
        uint64_t u;
    } packed;
    guarantee(sizeof(packed.d) == sizeof(packed.u));
    packed.d = as_num();
    // Mangle the value so that lexicographic ordering matches double ordering
    if (packed.u & (1ULL << 63)) {
        // If we have a negative double, flip all the bits.  Flipping the
        // highest bit causes the negative doubles to sort below the
        // positive doubles (which will also have their highest bit
        // flipped), and flipping all the other bits causes more negative
        // doubles to sort below less negative doubles.
        packed.u = ~packed.u;
    } else {
        // If we have a non-negative double, flip the highest bit so that it
        // sorts higher than all the negative doubles (which had their
        // highest bit flipped as well).
        packed.u ^= (1ULL << 63);
    }
    // The formatting here is sensitive.  Talk to mlucy before changing it.
    str_out->append(strprintf("%.*" PRIx64, static_cast<int>(sizeof(double)*2), packed.u));
    str_out->append(strprintf("#%" PR_RECONSTRUCTABLE_DOUBLE, as_num()));
}

void datum_t::binary_to_str_key(std::string *str_out) const {
    // We need to prepend "P" and append a character less than [a-zA-Z] so that
    // different pseudotypes sort correctly.
    const std::string binary_key_prefix("PBINARY:");
    const datum_string_t &key = as_binary();

    str_out->append(binary_key_prefix);
    size_t to_append = std::min(MAX_KEY_SIZE - str_out->size(), key.size());

    // Escape null bytes so we don't cause key ambiguity when used in an array
    // We do this by replacing \x00 with \x01\x01 and replacing \x01 with \x01\x02
    for (size_t i = 0; i < to_append; ++i) {
        if (key.data()[i] == '\x00') {
            str_out->append("\x01\x01");
        } else if (key.data()[i] == '\x01') {
            str_out->append("\x01\x02");
        } else {
            str_out->append(1, key.data()[i]);
        }
    }
}

void datum_t::str_to_str_key(std::string *str_out) const {
    r_sanity_check(get_type() == R_STR);
    str_out->append("S");
    size_t to_append = std::min(MAX_KEY_SIZE - str_out->size(), as_str().size());
    str_out->append(as_str().data(), to_append);
}

void datum_t::bool_to_str_key(std::string *str_out) const {
    r_sanity_check(get_type() == R_BOOL);
    str_out->append("B");
    if (as_bool()) {
        str_out->append("t");
    } else {
        str_out->append("f");
    }
}

// The key for an array is stored as a string of all its elements, each separated by a
//  null character, with another null character at the end to signify the end of the
//  array (this is necessary to prevent ambiguity when nested arrays are involved).
void datum_t::array_to_str_key(std::string *str_out) const {
    r_sanity_check(get_type() == R_ARRAY);
    str_out->append("A");

    const size_t sz = arr_size();
    for (size_t i = 0; i < sz && str_out->size() < MAX_KEY_SIZE; ++i) {
        datum_t item = get(i, NOTHROW);
        r_sanity_check(item.has());

        switch (item.get_type()) {
        case R_NUM: item.num_to_str_key(str_out); break;
        case R_STR: item.str_to_str_key(str_out); break;
        case R_BINARY: item.binary_to_str_key(str_out); break;
        case R_BOOL: item.bool_to_str_key(str_out); break;
        case R_ARRAY: item.array_to_str_key(str_out); break;
        case R_OBJECT:
            if (item.is_ptype()) {
                item.pt_to_str_key(str_out);
                break;
            }
            // fallthru
        case R_NULL:
            item.type_error(
                strprintf("Array keys can only contain numbers, strings, bools, "
                          " pseudotypes, or arrays (got %s of type %s).",
                          item.print().c_str(), item.get_type_name().c_str()));
            break;
        case UNINITIALIZED: // fallthru
        default:
            unreachable();
        }
        str_out->append(std::string(1, '\0'));
    }
}

int datum_t::pseudo_cmp(reql_version_t reql_version, const datum_t &rhs) const {
    r_sanity_check(is_ptype());
    if (get_type() == R_BINARY) {
        return as_binary().compare(rhs.as_binary());
    } else if (get_reql_type() == pseudo::time_string) {
        return pseudo::time_cmp(reql_version, *this, rhs);
    }

    rfail(base_exc_t::GENERIC, "Incomparable type %s.", get_type_name().c_str());
}

bool datum_t::pseudo_compares_as_obj() const {
    r_sanity_check(is_ptype());
    if (get_reql_type() == pseudo::geometry_string) {
        // We compare geometry by its object representation.
        // That's not especially meaningful, but works for indexing etc.
        return true;
    } else {
        return false;
    }
}

void datum_t::maybe_sanitize_ptype(const std::set<std::string> &allowed_pts) {
    if (is_ptype()) {
        std::string s = get_reql_type();
        if (s == pseudo::time_string) {
            pseudo::sanitize_time(this);
            return;
        }
        if (s == pseudo::literal_string) {
            rcheck(std_contains(allowed_pts, pseudo::literal_string),
                   base_exc_t::GENERIC,
                   "Stray literal keyword found: literal is only legal inside of "
                   "the object passed to merge or update and cannot nest inside "
                   "other literals.");
            pseudo::rcheck_literal_valid(this);
            return;
        }
        if (s == pseudo::geometry_string) {
            // Semantic geometry validation is handled separately whenever a
            // geometry object is created (or used, when necessary).
            // This just performs a basic syntactic check.
            pseudo::sanitize_geometry(this);
            return;
        }
        if (s == pseudo::binary_string) {
            // Sanitization cannot be performed when loading from a shared buffer.
            r_sanity_check(data.get_internal_type() == internal_type_t::R_OBJECT);
            // Clear the pseudotype data and convert it to binary data
            data = data_wrapper_t(construct_binary_t(),
                                  pseudo::decode_base64_ptype(*data.r_object));
            return;
        }
        rfail(base_exc_t::GENERIC,
              "Unknown $reql_type$ `%s`.", get_type_name().c_str());
    }
}

void datum_t::rcheck_is_ptype(const std::string s) const {
    rcheck(is_ptype(), base_exc_t::GENERIC,
           (s == ""
            ? strprintf("Not a pseudotype: `%s`.", trunc_print().c_str())
            : strprintf("Not a %s pseudotype: `%s`.",
                        s.c_str(),
                        trunc_print().c_str())));
}

datum_t datum_t::drop_literals(bool *encountered_literal_out) const {
    // drop_literals will never create arrays larger than those in the
    // existing datum; so checking (and thus threading the limits
    // parameter) is unnecessary here.
    const ql::configured_limits_t & limits = ql::configured_limits_t::unlimited;
    rassert(encountered_literal_out != NULL);

    const bool is_literal = is_ptype(pseudo::literal_string);
    if (is_literal) {
        datum_t val = get_field(pseudo::value_key, NOTHROW);
        if (val.has()) {
            bool encountered_literal;
            val = val.drop_literals(&encountered_literal);
            // Nested literals should have been caught on the higher QL levels.
            r_sanity_check(!encountered_literal);
        }
        *encountered_literal_out = true;
        return val;
    }

    // The result is either
    // - *this
    // - or if `need_to_copy` is true, `copied_result`
    bool need_to_copy = false;
    datum_t copied_result;

    if (get_type() == R_OBJECT) {
        datum_object_builder_t builder;

        const size_t sz = obj_size();
        for (size_t i = 0; i < sz; ++i) {
            auto pair = unchecked_get_pair(i);
            bool encountered_literal;
            datum_t val = pair.second.drop_literals(&encountered_literal);

            if (encountered_literal && !need_to_copy) {
                // We have encountered the first field with a literal.
                // This means we have to create a copy in `result_copy`.
                need_to_copy = true;
                // Copy everything up to now into the builder.
                for (size_t copy_i = 0; copy_i < i; ++copy_i) {
                    auto copy_pair = unchecked_get_pair(copy_i);
                    bool conflict = builder.add(copy_pair.first, copy_pair.second);
                    r_sanity_check(!conflict);
                }
            }

            if (need_to_copy) {
                if (val.has()) {
                    bool conflict = builder.add(pair.first, val);
                    r_sanity_check(!conflict);
                } else {
                    // If `pair.second` was a literal without a value, ignore it
                }
            }
        }

        copied_result = std::move(builder).to_datum();

    } else if (get_type() == R_ARRAY) {
        datum_array_builder_t builder(limits);

        const size_t sz = arr_size();
        for (size_t i = 0; i < sz; ++i) {
            bool encountered_literal;
            datum_t val = get(i).drop_literals(&encountered_literal);

            if (encountered_literal && !need_to_copy) {
                // We have encountered the first element with a literal.
                // This means we have to create a copy in `result_copy`.
                need_to_copy = true;
                // Copy everything up to now into the builder.
                for (size_t copy_i = 0; copy_i < i; ++copy_i) {
                    builder.add(get(copy_i));
                }
            }

            if (need_to_copy) {
                if (val.has()) {
                    builder.add(val);
                } else {
                    // If the element` was a literal without a value, ignore it
                }
            }
        }

        copied_result = std::move(builder).to_datum();
    }

    if (need_to_copy) {
        *encountered_literal_out = true;
        rassert(copied_result.has());
        return copied_result;
    } else {
        *encountered_literal_out = false;
        return *this;
    }
}

void datum_t::rcheck_valid_replace(datum_t old_val,
                                   datum_t orig_key,
                                   const datum_string_t &pkey) const {
    datum_t pk = get_field(pkey, NOTHROW);
    rcheck(pk.has(), base_exc_t::GENERIC,
           strprintf("Inserted object must have primary key `%s`:\n%s",
                     pkey.to_std().c_str(), print().c_str()));
    if (old_val.has()) {
        datum_t old_pk = orig_key;
        if (old_val.get_type() != R_NULL) {
            old_pk = old_val.get_field(pkey, NOTHROW);
            r_sanity_check(old_pk.has());
        }
        if (old_pk.has()) {
            rcheck(old_pk == pk, base_exc_t::GENERIC,
                   strprintf("Primary key `%s` cannot be changed (`%s` -> `%s`).",
                             pkey.to_std().c_str(), old_val.print().c_str(),
                             print().c_str()));
        }
    } else {
        r_sanity_check(!orig_key.has());
    }
}

std::string datum_t::print_primary() const {
    std::string s;
    switch (get_type()) {
    case R_NUM: num_to_str_key(&s); break;
    case R_STR: str_to_str_key(&s); break;
    case R_BINARY: binary_to_str_key(&s); break;
    case R_BOOL: bool_to_str_key(&s); break;
    case R_ARRAY: array_to_str_key(&s); break;
    case R_OBJECT:
        if (is_ptype()) {
            pt_to_str_key(&s);
            break;
        }
        // fallthru
    case R_NULL:
        type_error(strprintf(
            "Primary keys must be either a number, string, bool, pseudotype "
            "or array (got type %s):\n%s",
            get_type_name().c_str(), trunc_print().c_str()));
        break;
    case UNINITIALIZED: // fallthru
    default:
        unreachable();
    }

    if (s.size() > rdb_protocol::MAX_PRIMARY_KEY_SIZE) {
        rfail(base_exc_t::GENERIC,
              "Primary key too long (max %zu characters): %s",
              rdb_protocol::MAX_PRIMARY_KEY_SIZE - 1, print().c_str());
    }
    return s;
}

std::string datum_t::mangle_secondary(const std::string &secondary,
                                      const std::string &primary,
                                      const std::string &tag) {
    guarantee(secondary.size() < UINT8_MAX);
    guarantee(secondary.size() + primary.size() < UINT8_MAX);

    uint8_t pk_offset = static_cast<uint8_t>(secondary.size()),
            tag_offset = static_cast<uint8_t>(primary.size()) + pk_offset;

    std::string res = secondary + primary + tag +
           std::string(1, pk_offset) + std::string(1, tag_offset);
    guarantee(res.size() <= MAX_KEY_SIZE);
    return res;
}

std::string datum_t::encode_tag_num(uint64_t tag_num) {
    static_assert(sizeof(tag_num) == tag_size,
            "tag_size constant is assumed to be the size of a uint64_t.");
#ifndef BOOST_LITTLE_ENDIAN
    static_assert(false, "This piece of code will break on big-endian systems.");
#endif
    return std::string(reinterpret_cast<const char *>(&tag_num), tag_size);
}

std::string datum_t::compose_secondary(const std::string &secondary_key,
        const store_key_t &primary_key, boost::optional<uint64_t> tag_num) {
    std::string primary_key_string = key_to_unescaped_str(primary_key);

    if (primary_key_string.length() > rdb_protocol::MAX_PRIMARY_KEY_SIZE) {
        throw exc_t(base_exc_t::GENERIC,
            strprintf(
                "Primary key too long (max %zu characters): %s",
                rdb_protocol::MAX_PRIMARY_KEY_SIZE - 1,
                key_to_debug_str(primary_key).c_str()),
            NULL);
    }

    std::string tag_string;
    if (tag_num) {
        tag_string = encode_tag_num(tag_num.get());
    }

    const std::string truncated_secondary_key =
        secondary_key.substr(0, trunc_size(primary_key_string.length()));

    return mangle_secondary(truncated_secondary_key, primary_key_string, tag_string);
}

std::string datum_t::print_secondary(reql_version_t reql_version,
                                     const store_key_t &primary_key,
                                     boost::optional<uint64_t> tag_num) const {
    std::string secondary_key_string;

    // Reserve max key size to reduce reallocations
    secondary_key_string.reserve(MAX_KEY_SIZE);

    if (get_type() == R_NUM) {
        num_to_str_key(&secondary_key_string);
    } else if (get_type() == R_STR) {
        str_to_str_key(&secondary_key_string);
    } else if (get_type() == R_BINARY) {
        binary_to_str_key(&secondary_key_string);
    } else if (get_type() == R_BOOL) {
        bool_to_str_key(&secondary_key_string);
    } else if (get_type() == R_ARRAY) {
        array_to_str_key(&secondary_key_string);
    } else if (get_type() == R_OBJECT && is_ptype()) {
        pt_to_str_key(&secondary_key_string);
    } else {
        type_error(strprintf(
            "Secondary keys must be a number, string, bool, pseudotype, "
            "or array (got type %s):\n%s",
            get_type_name().c_str(), trunc_print().c_str()));
    }

    switch (reql_version) {
    case reql_version_t::v1_13:
        break;
    case reql_version_t::v1_14: // v1_15 is the same as v1_14
    case reql_version_t::v1_16_is_latest:
        secondary_key_string.append(1, '\x00');
        break;
    default:
        unreachable();
    }

    return compose_secondary(secondary_key_string, primary_key, tag_num);
}

void parse_secondary(const std::string &key,
                     components_t *components) THROWS_NOTHING {
    uint8_t start_of_tag = key[key.size() - 1],
            start_of_primary = key[key.size() - 2];

    guarantee(start_of_primary < start_of_tag);

    components->secondary = key.substr(0, start_of_primary);
    components->primary = key.substr(start_of_primary, start_of_tag - start_of_primary);

    std::string tag_str = key.substr(start_of_tag, key.size() - (start_of_tag + 2));
    if (tag_str.size() != 0) {
#ifndef BOOST_LITTLE_ENDIAN
        static_assert(false, "This piece of code will break on little endian systems.");
#endif
        components->tag_num = *reinterpret_cast<const uint64_t *>(tag_str.data());
    }
}

components_t datum_t::extract_all(const std::string &str) {
    components_t components;
    parse_secondary(str, &components);
    return components;
}

std::string datum_t::extract_primary(const std::string &secondary) {
    components_t components;
    parse_secondary(secondary, &components);
    return components.primary;
}

store_key_t datum_t::extract_primary(const store_key_t &secondary_key) {
    return store_key_t(extract_primary(key_to_unescaped_str(secondary_key)));
}

std::string datum_t::extract_secondary(const std::string &secondary) {
    components_t components;
    parse_secondary(secondary, &components);
    return components.secondary;
}

boost::optional<uint64_t> datum_t::extract_tag(const std::string &secondary) {
    components_t components;
    parse_secondary(secondary, &components);
    return components.tag_num;
}

boost::optional<uint64_t> datum_t::extract_tag(const store_key_t &key) {
    return extract_tag(key_to_unescaped_str(key));
}

// This function returns a store_key_t suitable for searching by a
// secondary-index.  This is needed because secondary indexes may be truncated,
// but the amount truncated depends on the length of the primary key.  Since we
// do not know how much was truncated, we have to truncate the maximum amount,
// then return all matches and filter them out later.
store_key_t datum_t::truncated_secondary() const {
    std::string s;
    if (get_type() == R_NUM) {
        num_to_str_key(&s);
    } else if (get_type() == R_STR) {
        str_to_str_key(&s);
    } else if (get_type() == R_BINARY) {
        binary_to_str_key(&s);
    } else if (get_type() == R_BOOL) {
        bool_to_str_key(&s);
    } else if (get_type() == R_ARRAY) {
        array_to_str_key(&s);
    } else if (get_type() == R_OBJECT && is_ptype()) {
        pt_to_str_key(&s);
    } else {
        type_error(strprintf(
            "Secondary keys must be a number, string, bool, pseudotype, "
            "or array (got %s of type %s).",
            print().c_str(), get_type_name().c_str()));
    }

    // Truncate the key if necessary
    if (s.length() >= max_trunc_size()) {
        s.erase(max_trunc_size());
    }

    return store_key_t(s);
}

void datum_t::check_type(type_t desired, const char *msg) const {
    rcheck_typed_target(
        this, get_type() == desired,
        (msg != NULL)
            ? std::string(msg)
            : strprintf("Expected type %s but found %s.",
                        raw_type_name(desired).c_str(), get_type_name().c_str()));
}
void datum_t::type_error(const std::string &msg) const {
    rfail_typed_target(this, "%s", msg.c_str());
}

bool datum_t::as_bool() const {
    if (get_type() == R_BOOL) {
        return data.r_bool;
    } else {
        return get_type() != R_NULL;
    }
}
double datum_t::as_num() const {
    check_type(R_NUM);
    return data.r_num;
}

bool number_as_integer(double d, int64_t *i_out) {
    static_assert(DBL_MANT_DIG == 53, "Doubles are wrong size.");

    if (min_dbl_int <= d && d <= max_dbl_int) {
        int64_t i = d;
        if (static_cast<double>(i) == d) {
            *i_out = i;
            return true;
        }
    }
    return false;
}

int64_t checked_convert_to_int(const rcheckable_t *target, double d) {
    int64_t i;
    if (number_as_integer(d, &i)) {
        return i;
    } else {
        rfail_target(target, base_exc_t::GENERIC,
                     "Number not an integer%s: %" PR_RECONSTRUCTABLE_DOUBLE,
                     d < min_dbl_int ? " (<-2^53)" :
                         d > max_dbl_int ? " (>2^53)" : "",
                     d);
    }
}

struct datum_rcheckable_t : public rcheckable_t {
    explicit datum_rcheckable_t(const datum_t *_datum) : datum(_datum) { }
    void runtime_fail(base_exc_t::type_t type,
                      const char *test, const char *file, int line,
                      std::string msg) const {
        datum->runtime_fail(type, test, file, line, msg);
    }
    const datum_t *datum;
};

int64_t datum_t::as_int() const {
    datum_rcheckable_t target(this);
    return checked_convert_to_int(&target, as_num());
}

const datum_string_t &datum_t::as_binary() const {
    check_type(R_BINARY);
    return data.r_str;
}

const datum_string_t &datum_t::as_str() const {
    check_type(R_STR);
    return data.r_str;
}

size_t datum_t::arr_size() const {
    check_type(R_ARRAY);
    if (data.get_internal_type() == internal_type_t::BUF_R_ARRAY) {
        return datum_get_array_size(data.buf_ref);
    } else {
        r_sanity_check(data.get_internal_type() == internal_type_t::R_ARRAY);
        return data.r_array->size();
    }
}

datum_t datum_t::get(size_t index, throw_bool_t throw_bool) const {
    // Calling `arr_size()` here also makes sure this this is actually an R_ARRAY.
    const size_t array_size = arr_size();
    if (index < array_size) {
        return unchecked_get(index);
    } else if (throw_bool == THROW) {
        rfail(base_exc_t::NON_EXISTENCE, "Index out of bounds: %zu", index);
    } else {
        return datum_t();
    }
}

datum_t datum_t::unchecked_get(size_t index) const {
    if (data.get_internal_type() == internal_type_t::BUF_R_ARRAY) {
        const size_t offset = datum_get_element_offset(data.buf_ref, index);
        return datum_deserialize_from_buf(data.buf_ref, offset);
    } else {
        r_sanity_check(data.get_internal_type() == internal_type_t::R_ARRAY);
        return (*data.r_array)[index];
    }
}

size_t datum_t::obj_size() const {
    check_type(R_OBJECT);
    if (data.get_internal_type() == internal_type_t::BUF_R_OBJECT) {
        return datum_get_array_size(data.buf_ref);
    } else {
        r_sanity_check(data.get_internal_type() == internal_type_t::R_OBJECT);
        return data.r_object->size();
    }
}

std::pair<datum_string_t, datum_t> datum_t::get_pair(size_t index) const {
    // Calling `obj_size()` here also makes sure this this is actually an R_OBJECT.
    guarantee(index < obj_size());
    return unchecked_get_pair(index);
}

std::pair<datum_string_t, datum_t> datum_t::unchecked_get_pair(size_t index) const {
    if (data.get_internal_type() == internal_type_t::BUF_R_OBJECT) {
        const size_t offset = datum_get_element_offset(data.buf_ref, index);
        return datum_deserialize_pair_from_buf(data.buf_ref, offset);
    } else {
        r_sanity_check(data.get_internal_type() == internal_type_t::R_OBJECT);
        return (*data.r_object)[index];
    }
}

datum_t datum_t::get_field(const datum_string_t &key, throw_bool_t throw_bool) const {
    // Use binary search on top of unchecked_get_pair()
    size_t range_beg = 0;
    // The obj_size() also makes sure that this has the right type (R_OBJECT)
    size_t range_end = obj_size();
    while (range_beg < range_end) {
        const size_t center = range_beg + ((range_end - range_beg) / 2);
        auto center_pair = unchecked_get_pair(center);
        const int cmp = key.compare(center_pair.first);
        if (cmp == 0) {
            // Found it
            return center_pair.second;
        } else if (cmp < 0) {
            range_end = center;
        } else {
            range_beg = center + 1;
        }
        rassert(range_beg <= range_end);
    }

    // Didn't find it
    if (throw_bool == THROW) {
        rfail(base_exc_t::NON_EXISTENCE,
              "No attribute `%s` in object:\n%s", key.to_std().c_str(), print().c_str());
    }
    return datum_t();
}

datum_t datum_t::get_field(const char *key, throw_bool_t throw_bool) const {
    return get_field(datum_string_t(key), throw_bool);
}

cJSON *datum_t::as_json_raw() const {
    switch (get_type()) {
    case R_NULL: return cJSON_CreateNull();
    case R_BINARY: return pseudo::encode_base64_ptype(as_binary()).release();
    case R_BOOL: return cJSON_CreateBool(as_bool());
    case R_NUM: return cJSON_CreateNumber(as_num());
    case R_STR: return cJSON_CreateStringN(as_str().data(), as_str().size());
    case R_ARRAY: {
        scoped_cJSON_t arr(cJSON_CreateArray());
        const size_t sz = arr_size();
        for (size_t i = 0; i < sz; ++i) {
            arr.AddItemToArray(unchecked_get(i).as_json_raw());
        }
        return arr.release();
    } break;
    case R_OBJECT: {
        scoped_cJSON_t obj(cJSON_CreateObject());
        const size_t sz = obj_size();
        for (size_t i = 0; i < sz; ++i) {
            auto pair = get_pair(i);
            obj.AddItemToObject(pair.first.data(), pair.first.size(),
                                pair.second.as_json_raw());
        }
        return obj.release();
    } break;
    case UNINITIALIZED: // fallthru
    default: unreachable();
    }
    unreachable();
}

scoped_cJSON_t datum_t::as_json() const {
    return scoped_cJSON_t(as_json_raw());
}

// TODO: make BINARY, STR, and OBJECT convertible to sequence?
counted_t<datum_stream_t>
datum_t::as_datum_stream(const protob_t<const Backtrace> &backtrace) const {
    switch (get_type()) {
    case R_NULL:   // fallthru
    case R_BINARY: // fallthru
    case R_BOOL:   // fallthru
    case R_NUM:    // fallthru
    case R_STR:    // fallthru
    case R_OBJECT: // fallthru
        type_error(strprintf("Cannot convert %s to SEQUENCE",
                             get_type_name().c_str()));
    case R_ARRAY:
        return make_counted<array_datum_stream_t>(*this, backtrace);
    case UNINITIALIZED: // fallthru
    default: unreachable();
    }
    unreachable();
}

void datum_t::replace_field(const datum_string_t &key, datum_t val) {
    check_type(R_OBJECT);
    r_sanity_check(val.has());
    // This function must only be used during sanitization, which is only performed
    // when not loading from a shared buffer.
    r_sanity_check(data.get_internal_type() == internal_type_t::R_OBJECT);

    auto key_cmp = [](const std::pair<datum_string_t, datum_t> &p1,
                      const datum_string_t &k2) -> bool {
        return p1.first < k2;
    };
    auto it = std::lower_bound(data.r_object->begin(), data.r_object->end(),
                               key, key_cmp);

    // The key must already exist
    r_sanity_check(it != data.r_object->end() && it->first == key);

    it->second = val;
}

datum_t datum_t::merge(const datum_t &rhs) const {
    if (get_type() != R_OBJECT || rhs.get_type() != R_OBJECT) {
        return rhs;
    }

    datum_object_builder_t d(*this);
    const size_t rhs_sz = rhs.obj_size();
    for (size_t i = 0; i < rhs_sz; ++i) {
        auto pair = rhs.unchecked_get_pair(i);
        datum_t sub_lhs = d.try_get(pair.first);
        bool is_literal = pair.second.is_ptype(pseudo::literal_string);

        if (pair.second.get_type() == R_OBJECT && sub_lhs.has() && !is_literal) {
            d.overwrite(pair.first, sub_lhs.merge(pair.second));
        } else {
            datum_t val =
                is_literal
                ? pair.second.get_field(pseudo::value_key, NOTHROW)
                : pair.second;
            if (val.has()) {
                // Since nested literal keywords are forbidden, this should be a no-op
                // if `is_literal == true`.
                bool encountered_literal;
                val = val.drop_literals(&encountered_literal);
                r_sanity_check(!encountered_literal || !is_literal);
            }
            if (val.has()) {
                d.overwrite(pair.first, val);
            } else {
                r_sanity_check(is_literal);
                UNUSED bool b = d.delete_field(pair.first);
            }
        }
    }
    return std::move(d).to_datum();
}

datum_t datum_t::merge(const datum_t &rhs,
                       merge_resoluter_t f,
                       const configured_limits_t &limits,
                       std::set<std::string> *conditions_out) const {
    datum_object_builder_t d(*this);
    const size_t rhs_sz = rhs.obj_size();
    for (size_t i = 0; i < rhs_sz; ++i) {
        auto pair = rhs.unchecked_get_pair(i);
        datum_t left = get_field(pair.first, NOTHROW);
        if (left.has()) {
            d.overwrite(pair.first, f(pair.first, left, pair.second, limits, conditions_out));
        } else {
            bool b = d.add(pair.first, pair.second);
            r_sanity_check(!b);
        }
    }
    return std::move(d).to_datum();
}

template<class T>
int derived_cmp(T a, T b) {
    if (a == b) return 0;
    return a < b ? -1 : 1;
}

int datum_t::v1_13_cmp(const datum_t &rhs) const {
    if (is_ptype() && !rhs.is_ptype()) {
        return 1;
    } else if (!is_ptype() && rhs.is_ptype()) {
        return -1;
    }

    if (get_type() != rhs.get_type()) {
        return derived_cmp(get_type(), rhs.get_type());
    }
    switch (get_type()) {
    case R_NULL: return 0;
    case R_BOOL: return derived_cmp(as_bool(), rhs.as_bool());
    case R_NUM: return derived_cmp(as_num(), rhs.as_num());
    case R_STR: return as_str().compare(rhs.as_str());
    case R_ARRAY: {
        size_t i;
        const size_t sz = arr_size();
        const size_t rhs_sz = rhs.arr_size();
        for (i = 0; i < sz; ++i) {
            if (i >= rhs_sz) return 1;
            int cmpval = unchecked_get(i).v1_13_cmp(rhs.unchecked_get(i));
            if (cmpval != 0) return cmpval;
        }
        guarantee(i <= rhs_sz);
        return i == rhs_sz ? 0 : -1;
    } unreachable();
    case R_OBJECT: {
        if (is_ptype() && !pseudo_compares_as_obj()) {
            if (get_reql_type() != rhs.get_reql_type()) {
                return derived_cmp(get_reql_type(), rhs.get_reql_type());
            }
            return pseudo_cmp(reql_version_t::v1_13, rhs);
        } else {
            size_t i = 0;
            size_t i2 = 0;
            const size_t sz = obj_size();
            const size_t rhs_sz = rhs.obj_size();
            while (i < sz && i2 < rhs_sz) {
                auto pair = unchecked_get_pair(i);
                auto pair2 = rhs.unchecked_get_pair(i2);
                int key_cmpval = pair.first.compare(pair2.first);
                if (key_cmpval != 0) {
                    return key_cmpval;
                }
                int val_cmpval = pair.second.v1_13_cmp(pair2.second);
                if (val_cmpval != 0) {
                    return val_cmpval;
                }
                ++i;
                ++i2;
            }
            if (i != sz) return 1;
            if (i2 != rhs_sz) return -1;
            return 0;
        }
    } unreachable();
    case R_BINARY: // This should be handled by the ptype code above
    case UNINITIALIZED: // fallthru
    default: unreachable();
    }
}

int datum_t::cmp(reql_version_t reql_version, const datum_t &rhs) const {
    switch (reql_version) {
    case reql_version_t::v1_13:
        return v1_13_cmp(rhs);
    case reql_version_t::v1_14: // v1_15 is the same as v1_14
    case reql_version_t::v1_16_is_latest:
        return modern_cmp(rhs);
    default:
        unreachable();
    }
}

int datum_t::modern_cmp(const datum_t &rhs) const {
    bool lhs_ptype = is_ptype() && !pseudo_compares_as_obj();
    bool rhs_ptype = rhs.is_ptype() && !rhs.pseudo_compares_as_obj();
    if (lhs_ptype && rhs_ptype) {
        if (get_reql_type() != rhs.get_reql_type()) {
            return derived_cmp(get_reql_type(), rhs.get_reql_type());
        }
        return pseudo_cmp(reql_version_t::v1_16_is_latest, rhs);
    } else if (lhs_ptype || rhs_ptype) {
        return derived_cmp(get_type_name(), rhs.get_type_name());
    }

    if (get_type() != rhs.get_type()) {
        return derived_cmp(get_type(), rhs.get_type());
    }
    switch (get_type()) {
    case R_NULL: return 0;
    case R_BOOL: return derived_cmp(as_bool(), rhs.as_bool());
    case R_NUM: return derived_cmp(as_num(), rhs.as_num());
    case R_STR: return as_str().compare(rhs.as_str());
    case R_ARRAY: {
        size_t i;
        const size_t sz = arr_size();
        const size_t rhs_sz = rhs.arr_size();
        for (i = 0; i < sz; ++i) {
            if (i >= rhs_sz) return 1;
            int cmpval = unchecked_get(i).modern_cmp(rhs.unchecked_get(i));
            if (cmpval != 0) return cmpval;
        }
        guarantee(i <= rhs_sz);
        return i == rhs_sz ? 0 : -1;
    } unreachable();
    case R_OBJECT: {
        size_t i = 0;
        size_t i2 = 0;
        const size_t sz = obj_size();
        const size_t rhs_sz = rhs.obj_size();
        while (i < sz && i2 < rhs_sz) {
            auto pair = unchecked_get_pair(i);
            auto pair2 = rhs.unchecked_get_pair(i2);
            int key_cmpval = pair.first.compare(pair2.first);
            if (key_cmpval != 0) {
                return key_cmpval;
            }
            int val_cmpval = pair.second.modern_cmp(pair2.second);
            if (val_cmpval != 0) {
                return val_cmpval;
            }
            ++i;
            ++i2;
        }
        if (i != sz) return 1;
        if (i2 != rhs_sz) return -1;
        return 0;
    } unreachable();
    case R_BINARY: // This should be handled by the ptype code above
    case UNINITIALIZED: // fallthru
    default: unreachable();
    }
}

bool datum_t::operator==(const datum_t &rhs) const { return modern_cmp(rhs) == 0; }
bool datum_t::operator!=(const datum_t &rhs) const { return modern_cmp(rhs) != 0; }
bool datum_t::compare_lt(reql_version_t reql_version, const datum_t &rhs) const {
    return cmp(reql_version, rhs) < 0;
}
bool datum_t::compare_gt(reql_version_t reql_version, const datum_t &rhs) const {
    return cmp(reql_version, rhs) > 0;
}

void datum_t::runtime_fail(base_exc_t::type_t exc_type,
                           const char *test, const char *file, int line,
                           std::string msg) const {
    ql::runtime_fail(exc_type, test, file, line, msg);
}

datum_t to_datum(const Datum *d, const configured_limits_t &limits,
                 reql_version_t reql_version) {
    switch (d->type()) {
    case Datum::R_NULL: {
        return datum_t::null();
    } break;
    case Datum::R_BOOL: {
        return datum_t::boolean(d->r_bool());
    } break;
    case Datum::R_NUM: {
        return datum_t(d->r_num());
    } break;
    case Datum::R_STR: {
        fail_if_invalid(reql_version, d->r_str());
        return datum_t(datum_string_t(d->r_str()));
    } break;
    case Datum::R_JSON: {
        fail_if_invalid(reql_version, d->r_str());
        scoped_cJSON_t cjson(cJSON_Parse(d->r_str().c_str()));
        return to_datum(cjson.get(), limits, reql_version);
    } break;
    case Datum::R_ARRAY: {
        datum_array_builder_t out(limits);
        out.reserve(d->r_array_size());
        for (int i = 0, e = d->r_array_size(); i < e; ++i) {
            out.add(to_datum(&d->r_array(i), limits, reql_version));
        }
        return std::move(out).to_datum();
    } break;
    case Datum::R_OBJECT: {
        std::map<datum_string_t, datum_t> map;
        const int count = d->r_object_size();
        for (int i = 0; i < count; ++i) {
            const Datum_AssocPair *ap = &d->r_object(i);
            datum_string_t key(ap->key());
            datum_t::check_str_validity(key);
            fail_if_invalid(reql_version, ap->key());
            auto res = map.insert(std::make_pair(key,
                                                 to_datum(&ap->val(), limits,
                                                          reql_version)));
            rcheck_datum(res.second, base_exc_t::GENERIC,
                         strprintf("Duplicate key %s in object.", key.to_std().c_str()));
        }
        const std::set<std::string> pts = { pseudo::literal_string };
        return datum_t(std::move(map), pts);
    } break;
    default: unreachable();
    }
}

size_t datum_t::max_trunc_size() {
    return trunc_size(rdb_protocol::MAX_PRIMARY_KEY_SIZE);
}

size_t datum_t::trunc_size(size_t primary_key_size) {
    //The 2 in this function is necessary because of the offsets which are
    //included at the end of the key so that we can extract the primary key and
    //the tag num from secondary keys.
    return MAX_KEY_SIZE - primary_key_size - tag_size - 2;
}

bool datum_t::key_is_truncated(const store_key_t &key) {
    std::string key_str = key_to_unescaped_str(key);
    if (extract_tag(key_str)) {
        return key.size() == MAX_KEY_SIZE;
    } else {
        return key.size() == MAX_KEY_SIZE - tag_size;
    }
}

void datum_t::write_to_protobuf(Datum *d, use_json_t use_json) const {
    switch (use_json) {
    case use_json_t::NO: {
        switch (get_type()) {
        case R_NULL: {
            d->set_type(Datum::R_NULL);
        } break;
        case R_BINARY: {
            pseudo::write_binary_to_protobuf(d, data.r_str);
        } break;
        case R_BOOL: {
            d->set_type(Datum::R_BOOL);
            d->set_r_bool(data.r_bool);
        } break;
        case R_NUM: {
            d->set_type(Datum::R_NUM);
            // so we can use `isfinite` in a GCC 4.4.3-compatible way
            using namespace std;  // NOLINT(build/namespaces)
            r_sanity_check(isfinite(data.r_num));
            d->set_r_num(data.r_num);
        } break;
        case R_STR: {
            d->set_type(Datum::R_STR);
            d->set_r_str(data.r_str.data(), data.r_str.size());
        } break;
        case R_ARRAY: {
            d->set_type(Datum::R_ARRAY);
            const size_t sz = arr_size();
            for (size_t i = 0; i < sz; ++i) {
                get(i).write_to_protobuf(d->add_r_array(), use_json);
            }
        } break;
        case R_OBJECT: {
            d->set_type(Datum::R_OBJECT);
            // We use the opposite order so that things print the way we expect.
            for (size_t i = obj_size(); i > 0; --i) {
                Datum_AssocPair *ap = d->add_r_object();
                auto pair = get_pair(i-1);
                ap->set_key(pair.first.data(), pair.first.size());
                pair.second.write_to_protobuf(ap->mutable_val(), use_json);
            }
        } break;
        case UNINITIALIZED: // fallthru
        default: unreachable();
        }
    } break;
    case use_json_t::YES: {
        d->set_type(Datum::R_JSON);
        d->set_r_str(as_json().PrintUnformatted());
    } break;
    default: unreachable();
    }
}

// `key` is unused because this is passed to `datum_t::merge`, which takes a
// generic conflict resolution function, but this particular conflict resolution
// function doesn't care about they key (although we could add some
// error-checking using the key in the future).
datum_t stats_merge(UNUSED const datum_string_t &key,
                    datum_t l,
                    datum_t r,
                    const configured_limits_t &limits,
                    std::set<std::string> *conditions) {
    if (l.get_type() == datum_t::R_NUM && r.get_type() == datum_t::R_NUM) {
        return datum_t(l.as_num() + r.as_num());
    } else if (l.get_type() == datum_t::R_ARRAY && r.get_type() == datum_t::R_ARRAY) {
        const size_t l_sz = l.arr_size();
        const size_t r_sz = r.arr_size();
        if (l_sz + r_sz > limits.array_size_limit()) {
            conditions->insert(strprintf("Too many changes, array truncated to %zu.", limits.array_size_limit()));
            datum_array_builder_t arr(limits);
            size_t so_far = 0;
            for (size_t i = 0; i < l_sz && so_far < limits.array_size_limit(); ++i, ++so_far) {
                arr.add(l.get(i));
            }
            for (size_t i = 0; i < r_sz && so_far < limits.array_size_limit(); ++i, ++so_far) {
                arr.add(r.get(i));
            }
            return std::move(arr).to_datum();
        } else {
            datum_array_builder_t arr(limits);
            for (size_t i = 0; i < l_sz; ++i) {
                arr.add(l.get(i));
            }
            for (size_t i = 0; i < r_sz; ++i) {
                arr.add(r.get(i));
            }
            return std::move(arr).to_datum();
        }
    }

    // Merging a string is left-preferential, which is just a no-op.
    rcheck_datum(
        l.get_type() == datum_t::R_STR && r.get_type() == datum_t::R_STR,
        base_exc_t::GENERIC,
        strprintf("Cannot merge statistics `%s` (type %s) and `%s` (type %s).",
                  l.trunc_print().c_str(), l.get_type_name().c_str(),
                  r.trunc_print().c_str(), r.get_type_name().c_str()));
    return l;
}

datum_object_builder_t::datum_object_builder_t(const datum_t &copy_from) {
    const size_t copy_from_sz = copy_from.obj_size();
    for (size_t i = 0; i < copy_from_sz; ++i) {
        map.insert(copy_from.get_pair(i));
    }
}

bool datum_object_builder_t::add(const datum_string_t &key, datum_t val) {
    datum_t::check_str_validity(key);
    r_sanity_check(val.has());
    auto res = map.insert(std::make_pair(key, std::move(val)));
    // Return _false_ if the insertion actually happened.  Because we are being
    // backwards to the C++ convention.
    return !res.second;
}

bool datum_object_builder_t::add(const char *key, datum_t val) {
    return add(datum_string_t(key), val);
}

void datum_object_builder_t::overwrite(const datum_string_t &key,
                                       datum_t val) {
    datum_t::check_str_validity(key);
    r_sanity_check(val.has());
    map[key] = std::move(val);
}

void datum_object_builder_t::overwrite(const char *key,
                                       datum_t val) {
    return overwrite(datum_string_t(key), val);
}

void datum_object_builder_t::add_warning(const char *msg, const configured_limits_t &limits) {
    datum_t *warnings_entry = &map[warnings_field];
    if (warnings_entry->has()) {
        // assume here that the warnings array will "always" be small.
        const size_t warnings_entry_sz = warnings_entry->arr_size();
        for (size_t i = 0; i < warnings_entry_sz; ++i) {
            if (warnings_entry->get(i).as_str() == msg) return;
        }
        rcheck_datum(warnings_entry_sz + 1 <= limits.array_size_limit(),
            base_exc_t::GENERIC,
            strprintf("Warnings would exceed array size limit %zu; increase it to see warnings", limits.array_size_limit()));
        datum_array_builder_t out(*warnings_entry, limits);
        out.add(datum_t(msg));
        *warnings_entry = std::move(out).to_datum();
    } else {
        datum_array_builder_t out(limits);
        out.add(datum_t(msg));
        *warnings_entry = std::move(out).to_datum();
    }
}

void datum_object_builder_t::add_warnings(const std::set<std::string> &msgs, const configured_limits_t &limits) {
    if (msgs.empty()) return;
    datum_t *warnings_entry = &map[warnings_field];
    if (warnings_entry->has()) {
        rcheck_datum(warnings_entry->arr_size() + msgs.size() <= limits.array_size_limit(),
            base_exc_t::GENERIC,
            strprintf("Warnings would exceed array size limit %zu; increase it to see warnings", limits.array_size_limit()));
        datum_array_builder_t out(*warnings_entry, limits);
        for (auto const & msg : msgs) {
            bool seen = false;
            // assume here that the warnings array will "always" be small.
            const size_t warnings_entry_sz = warnings_entry->arr_size();
            for (size_t i = 0; i < warnings_entry_sz; ++i) {
                if (warnings_entry->get(i).as_str() == msg.c_str()) {
                    seen = true;
                    break;
                }
            }
            if (!seen) out.add(datum_t(msg.c_str()));
        }
        *warnings_entry = std::move(out).to_datum();
    } else {
        datum_array_builder_t out(limits);
        for (auto const & msg : msgs) {
            out.add(datum_t(msg.c_str()));
        }
        *warnings_entry = std::move(out).to_datum();
    }
}

void datum_object_builder_t::add_error(const char *msg) {
    // Insert or update the "errors" entry.
    {
        datum_t *errors_entry = &map[errors_field];
        double ecount = (errors_entry->has() ? (*errors_entry).as_num() : 0) + 1;
        *errors_entry = datum_t(ecount);
    }

    // If first_error already exists, nothing gets inserted.
    map.insert(std::make_pair(first_error_field, datum_t(msg)));
}

MUST_USE bool datum_object_builder_t::delete_field(const datum_string_t &key) {
    return 0 != map.erase(key);
}

MUST_USE bool datum_object_builder_t::delete_field(const char *key) {
    return delete_field(datum_string_t(key));
}


datum_t datum_object_builder_t::at(const datum_string_t &key) const {
    return map.at(key);
}

datum_t datum_object_builder_t::try_get(const datum_string_t &key) const {
    auto it = map.find(key);
    return it == map.end() ? datum_t() : it->second;
}

datum_t datum_object_builder_t::to_datum() RVALUE_THIS {
    return datum_t(std::move(map));
}

datum_t datum_object_builder_t::to_datum(
        const std::set<std::string> &permissible_ptypes) RVALUE_THIS {
    return datum_t(std::move(map), permissible_ptypes);
}

datum_array_builder_t::datum_array_builder_t(const datum_t &copy_from,
                                             const configured_limits_t &_limits)
    : limits(_limits) {
    const size_t copy_from_sz = copy_from.arr_size();
    vector.reserve(copy_from_sz);
    for (size_t i = 0; i < copy_from_sz; ++i) {
        vector.push_back(copy_from.get(i));
    }
    rcheck_array_size_datum(vector, limits, base_exc_t::GENERIC);
}

void datum_array_builder_t::reserve(size_t n) { vector.reserve(n); }

void datum_array_builder_t::add(datum_t val) {
    vector.push_back(std::move(val));
    rcheck_array_size_datum(vector, limits, base_exc_t::GENERIC);
}

void datum_array_builder_t::change(size_t index, datum_t val) {
    rcheck_datum(index < vector.size(),
                 base_exc_t::NON_EXISTENCE,
                 strprintf("Index `%zu` out of bounds for array of size: `%zu`.",
                           index, vector.size()));
    vector[index] = std::move(val);
}

void datum_array_builder_t::insert(reql_version_t reql_version, size_t index,
                                   datum_t val) {
    rcheck_datum(index <= vector.size(),
                 base_exc_t::NON_EXISTENCE,
                 strprintf("Index `%zu` out of bounds for array of size: `%zu`.",
                           index, vector.size()));
    vector.insert(vector.begin() + index, std::move(val));

    switch (reql_version) {
    case reql_version_t::v1_13:
        break;
    case reql_version_t::v1_14: // v1_15 is the same as v1_14
    case reql_version_t::v1_16_is_latest:
        rcheck_array_size_datum(vector, limits, base_exc_t::GENERIC);
        break;
    default:
        unreachable();
    }
}

void datum_array_builder_t::splice(reql_version_t reql_version, size_t index,
                                   datum_t values) {
    rcheck_datum(index <= vector.size(),
                 base_exc_t::NON_EXISTENCE,
                 strprintf("Index `%zu` out of bounds for array of size: `%zu`.",
                           index, vector.size()));

    // First copy the values into a vector so vector.insert() can know the number
    // of elements being inserted.
    std::vector<datum_t> arr;
    const size_t values_sz = values.arr_size();
    arr.reserve(values_sz);
    for (size_t i = 0; i < values_sz; ++i) {
        arr.push_back(values.get(i));
    }
    vector.insert(vector.begin() + index,
                  std::make_move_iterator(arr.begin()),
                  std::make_move_iterator(arr.end()));

    switch (reql_version) {
    case reql_version_t::v1_13:
        break;
    case reql_version_t::v1_14: // v1_15 is the same as v1_14
    case reql_version_t::v1_16_is_latest:
        rcheck_array_size_datum(vector, limits, base_exc_t::GENERIC);
        break;
    default:
        unreachable();
    }
}

void datum_array_builder_t::erase_range(reql_version_t reql_version,
                                        size_t start, size_t end) {

    // See https://github.com/rethinkdb/rethinkdb/issues/2696 about the backwards
    // compatible implementation for v1_13.

    switch (reql_version) {
    case reql_version_t::v1_13:
        rcheck_datum(start < vector.size(),
                     base_exc_t::NON_EXISTENCE,
                     strprintf("Index `%zu` out of bounds for array of size: `%zu`.",
                               start, vector.size()));
        break;
    case reql_version_t::v1_14: // v1_15 is the same as v1_14
    case reql_version_t::v1_16_is_latest:
        rcheck_datum(start <= vector.size(),
                     base_exc_t::NON_EXISTENCE,
                     strprintf("Index `%zu` out of bounds for array of size: `%zu`.",
                               start, vector.size()));
        break;
    default:
        unreachable();
    }


    rcheck_datum(end <= vector.size(),
                 base_exc_t::NON_EXISTENCE,
                 strprintf("Index `%zu` out of bounds for array of size: `%zu`.",
                           end, vector.size()));
    rcheck_datum(start <= end,
                 base_exc_t::GENERIC,
                 strprintf("Start index `%zu` is greater than end index `%zu`.",
                           start, end));
    vector.erase(vector.begin() + start, vector.begin() + end);
}

void datum_array_builder_t::erase(size_t index) {
    rcheck_datum(index < vector.size(),
                 base_exc_t::NON_EXISTENCE,
                 strprintf("Index `%zu` out of bounds for array of size: `%zu`.",
                           index, vector.size()));
    vector.erase(vector.begin() + index);
}

datum_t datum_array_builder_t::to_datum() RVALUE_THIS {
    // We call the non-checking constructor.  See
    // https://github.com/rethinkdb/rethinkdb/issues/2697 for more information --
    // insert and splice don't check the array size limit, because of a bug (as
    // reported in the issue).  This maintains that broken ReQL behavior because of
    // the generic reasons you would do so: secondary index compatibility after an
    // upgrade.
    return datum_t(std::move(vector), datum_t::no_array_size_limit_check_t());
}

datum_range_t::datum_range_t()
    : left_bound_type(key_range_t::none), right_bound_type(key_range_t::none) { }
datum_range_t::datum_range_t(
    datum_t _left_bound, key_range_t::bound_t _left_bound_type,
    datum_t _right_bound, key_range_t::bound_t _right_bound_type)
    : left_bound(_left_bound), right_bound(_right_bound),
      left_bound_type(_left_bound_type), right_bound_type(_right_bound_type) { }
datum_range_t::datum_range_t(datum_t val)
    : left_bound(val), right_bound(val),
      left_bound_type(key_range_t::closed), right_bound_type(key_range_t::closed) { }

datum_range_t datum_range_t::universe()  {
    return datum_range_t(datum_t(), key_range_t::open,
                         datum_t(), key_range_t::open);
}
bool datum_range_t::is_universe() const {
    return !left_bound.has() && !right_bound.has()
        && left_bound_type == key_range_t::open && right_bound_type == key_range_t::open;
}

bool datum_range_t::contains(reql_version_t reql_version,
                             datum_t val) const {
    return (!left_bound.has()
            || left_bound.compare_lt(reql_version, val)
            || (left_bound == val && left_bound_type == key_range_t::closed))
        && (!right_bound.has()
            || right_bound.compare_gt(reql_version, val)
            || (right_bound == val && right_bound_type == key_range_t::closed));
}

key_range_t datum_range_t::to_primary_keyrange() const {
    return key_range_t(
        left_bound_type,
        left_bound.has()
            ? store_key_t(left_bound.print_primary())
            : store_key_t::min(),
        right_bound_type,
        right_bound.has()
            ? store_key_t(right_bound.print_primary())
            : store_key_t::max());
}

key_range_t datum_range_t::to_sindex_keyrange() const {
    return rdb_protocol::sindex_key_range(
        left_bound.has()
            ? store_key_t(left_bound.truncated_secondary())
            : store_key_t::min(),
        right_bound.has()
            ? store_key_t(right_bound.truncated_secondary())
            : store_key_t::max());
}

datum_range_t datum_range_t::with_left_bound(datum_t d, key_range_t::bound_t type) {
    return datum_range_t(d, type, right_bound, right_bound_type);
}

datum_range_t datum_range_t::with_right_bound(datum_t d, key_range_t::bound_t type) {
    return datum_range_t(left_bound, left_bound_type, d, type);
}

void debug_print(printf_buffer_t *buf, const datum_t &d) {
    switch (d.data.get_internal_type()) {
    case datum_t::internal_type_t::UNINITIALIZED:
        buf->appendf("d/uninitialized");
        break;
    case datum_t::internal_type_t::R_ARRAY:
        buf->appendf("d/array");
        if (!d.data.r_array.has()) {
            buf->appendf("(null!?)");
        } else {
            debug_print(buf, *d.data.r_array);
        }
        break;
    case datum_t::internal_type_t::R_BINARY:
        buf->appendf("d/binary(");
        debug_print(buf, d.data.r_str);
        buf->appendf(")");
        break;
    case datum_t::internal_type_t::R_BOOL:
        buf->appendf("d/%s", d.data.r_bool ? "true" : "false");
        break;
    case datum_t::internal_type_t::R_NULL:
        buf->appendf("d/null");
        break;
    case datum_t::internal_type_t::R_NUM:
        buf->appendf("d/number(%" PR_RECONSTRUCTABLE_DOUBLE ")", d.data.r_num);
        break;
    case datum_t::internal_type_t::R_OBJECT:
        buf->appendf("d/object");
        debug_print(buf, *d.data.r_object);
        break;
    case datum_t::internal_type_t::R_STR:
        buf->appendf("d/string(");
        debug_print(buf, d.data.r_str);
        buf->appendf(")");
        break;
    case datum_t::internal_type_t::BUF_R_ARRAY:
        buf->appendf("d/buf_r_array(...)");
        break;
    case datum_t::internal_type_t::BUF_R_OBJECT:
        buf->appendf("d/buf_r_object(...)");
        break;
    default:
        buf->appendf("datum/garbage{internal_type=%d}", d.data.get_internal_type());
        break;
    }
}

ARCHIVE_PRIM_MAKE_RANGED_SERIALIZABLE(key_range_t::bound_t, int8_t,
                                      key_range_t::open, key_range_t::none);
RDB_IMPL_SERIALIZABLE_4(
        datum_range_t, left_bound, right_bound, left_bound_type, right_bound_type);
INSTANTIATE_SERIALIZABLE_FOR_CLUSTER(datum_range_t);

} // namespace ql
