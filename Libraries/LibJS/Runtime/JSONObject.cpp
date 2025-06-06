/*
 * Copyright (c) 2020, Matthew Olsson <mattco@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Function.h>
#include <AK/JsonArray.h>
#include <AK/JsonObject.h>
#include <AK/JsonParser.h>
#include <AK/StringBuilder.h>
#include <AK/TypeCasts.h>
#include <AK/Utf16View.h>
#include <AK/Utf8View.h>
#include <LibJS/Runtime/AbstractOperations.h>
#include <LibJS/Runtime/Array.h>
#include <LibJS/Runtime/BigIntObject.h>
#include <LibJS/Runtime/BooleanObject.h>
#include <LibJS/Runtime/Error.h>
#include <LibJS/Runtime/FunctionObject.h>
#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/Runtime/JSONObject.h>
#include <LibJS/Runtime/NumberObject.h>
#include <LibJS/Runtime/Object.h>
#include <LibJS/Runtime/RawJSONObject.h>
#include <LibJS/Runtime/StringObject.h>
#include <LibJS/Runtime/ValueInlines.h>

namespace JS {

GC_DEFINE_ALLOCATOR(JSONObject);

JSONObject::JSONObject(Realm& realm)
    : Object(ConstructWithPrototypeTag::Tag, realm.intrinsics().object_prototype())
{
}

void JSONObject::initialize(Realm& realm)
{
    auto& vm = this->vm();
    Base::initialize(realm);
    u8 attr = Attribute::Writable | Attribute::Configurable;
    define_native_function(realm, vm.names.stringify, stringify, 3, attr);
    define_native_function(realm, vm.names.parse, parse, 2, attr);
    define_native_function(realm, vm.names.rawJSON, raw_json, 1, attr);
    define_native_function(realm, vm.names.isRawJSON, is_raw_json, 1, attr);

    // 25.5.3 JSON [ @@toStringTag ], https://tc39.es/ecma262/#sec-json-@@tostringtag
    define_direct_property(vm.well_known_symbol_to_string_tag(), PrimitiveString::create(vm, "JSON"_string), Attribute::Configurable);
}

// 25.5.2 JSON.stringify ( value [ , replacer [ , space ] ] ), https://tc39.es/ecma262/#sec-json.stringify
ThrowCompletionOr<Optional<String>> JSONObject::stringify_impl(VM& vm, Value value, Value replacer, Value space)
{
    auto& realm = *vm.current_realm();

    StringifyState state;

    if (replacer.is_object()) {
        if (replacer.as_object().is_function()) {
            state.replacer_function = &replacer.as_function();
        } else {
            auto is_array = TRY(replacer.is_array(vm));
            if (is_array) {
                auto& replacer_object = replacer.as_object();
                auto replacer_length = TRY(length_of_array_like(vm, replacer_object));
                Vector<String> list;
                for (size_t i = 0; i < replacer_length; ++i) {
                    auto replacer_value = TRY(replacer_object.get(i));
                    Optional<String> item;
                    if (replacer_value.is_string()) {
                        item = replacer_value.as_string().utf8_string();
                    } else if (replacer_value.is_number()) {
                        item = MUST(replacer_value.to_string(vm));
                    } else if (replacer_value.is_object()) {
                        auto& value_object = replacer_value.as_object();
                        if (is<StringObject>(value_object) || is<NumberObject>(value_object))
                            item = TRY(replacer_value.to_string(vm));
                    }
                    if (item.has_value() && !list.contains_slow(*item)) {
                        list.append(*item);
                    }
                }
                state.property_list = list;
            }
        }
    }

    if (space.is_object()) {
        auto& space_object = space.as_object();
        if (is<NumberObject>(space_object))
            space = TRY(space.to_number(vm));
        else if (is<StringObject>(space_object))
            space = TRY(space.to_primitive_string(vm));
    }

    if (space.is_number()) {
        auto space_mv = MUST(space.to_integer_or_infinity(vm));
        space_mv = min(10, space_mv);
        state.gap = space_mv < 1 ? String {} : MUST(String::repeated(' ', space_mv));
    } else if (space.is_string()) {
        auto string = space.as_string().utf8_string();
        if (string.bytes().size() <= 10)
            state.gap = string;
        else
            state.gap = MUST(string.substring_from_byte_offset(0, 10));
    } else {
        state.gap = String {};
    }

    auto wrapper = Object::create(realm, realm.intrinsics().object_prototype());
    MUST(wrapper->create_data_property_or_throw(String {}, value));
    return serialize_json_property(vm, state, String {}, wrapper);
}

// 25.5.2 JSON.stringify ( value [ , replacer [ , space ] ] ), https://tc39.es/ecma262/#sec-json.stringify
JS_DEFINE_NATIVE_FUNCTION(JSONObject::stringify)
{
    if (!vm.argument_count())
        return js_undefined();

    auto value = vm.argument(0);
    auto replacer = vm.argument(1);
    auto space = vm.argument(2);

    auto maybe_string = TRY(stringify_impl(vm, value, replacer, space));
    if (!maybe_string.has_value())
        return js_undefined();

    return PrimitiveString::create(vm, maybe_string.release_value());
}

// 25.5.2.1 SerializeJSONProperty ( state, key, holder ), https://tc39.es/ecma262/#sec-serializejsonproperty
// 1.4.1 SerializeJSONProperty ( state, key, holder ), https://tc39.es/proposal-json-parse-with-source/#sec-serializejsonproperty
ThrowCompletionOr<Optional<String>> JSONObject::serialize_json_property(VM& vm, StringifyState& state, PropertyKey const& key, Object* holder)
{
    // 1. Let value be ? Get(holder, key).
    auto value = TRY(holder->get(key));

    // 2. If Type(value) is Object or BigInt, then
    if (value.is_object() || value.is_bigint()) {
        // a. Let toJSON be ? GetV(value, "toJSON").
        auto to_json = TRY(value.get(vm, vm.names.toJSON));

        // b. If IsCallable(toJSON) is true, then
        if (to_json.is_function()) {
            // i. Set value to ? Call(toJSON, value, « key »).
            value = TRY(call(vm, to_json.as_function(), value, PrimitiveString::create(vm, key.to_string())));
        }
    }

    // 3. If state.[[ReplacerFunction]] is not undefined, then
    if (state.replacer_function) {
        // a. Set value to ? Call(state.[[ReplacerFunction]], holder, « key, value »).
        value = TRY(call(vm, *state.replacer_function, holder, PrimitiveString::create(vm, key.to_string()), value));
    }

    // 4. If Type(value) is Object, then
    if (value.is_object()) {
        auto& value_object = value.as_object();

        // a. If value has an [[IsRawJSON]] internal slot, then
        if (is<RawJSONObject>(value_object)) {
            // i. Return ! Get(value, "rawJSON").
            return MUST(value_object.get(vm.names.rawJSON)).as_string().utf8_string();
        }
        // b. If value has a [[NumberData]] internal slot, then
        if (is<NumberObject>(value_object)) {
            // i. Set value to ? ToNumber(value).
            value = TRY(value.to_number(vm));
        }
        // c. Else if value has a [[StringData]] internal slot, then
        else if (is<StringObject>(value_object)) {
            // i. Set value to ? ToString(value).
            value = TRY(value.to_primitive_string(vm));
        }
        // d. Else if value has a [[BooleanData]] internal slot, then
        else if (is<BooleanObject>(value_object)) {
            // i. Set value to value.[[BooleanData]].
            value = Value(static_cast<BooleanObject&>(value_object).boolean());
        }
        // e. Else if value has a [[BigIntData]] internal slot, then
        else if (is<BigIntObject>(value_object)) {
            // i. Set value to value.[[BigIntData]].
            value = Value(&static_cast<BigIntObject&>(value_object).bigint());
        }
    }

    // 5. If value is null, return "null".
    if (value.is_null())
        return "null"_string;

    // 6. If value is true, return "true".
    // 7. If value is false, return "false".
    if (value.is_boolean())
        return value.as_bool() ? "true"_string : "false"_string;

    // 8. If Type(value) is String, return QuoteJSONString(value).
    if (value.is_string())
        return quote_json_string(value.as_string().utf8_string());

    // 9. If Type(value) is Number, then
    if (value.is_number()) {
        // a. If value is finite, return ! ToString(value).
        if (value.is_finite_number())
            return MUST(value.to_string(vm));

        // b. Return "null".
        return "null"_string;
    }

    // 10. If Type(value) is BigInt, throw a TypeError exception.
    if (value.is_bigint())
        return vm.throw_completion<TypeError>(ErrorType::JsonBigInt);

    // 11. If Type(value) is Object and IsCallable(value) is false, then
    if (value.is_object() && !value.is_function()) {
        // a. Let isArray be ? IsArray(value).
        auto is_array = TRY(value.is_array(vm));

        // b. If isArray is true, return ? SerializeJSONArray(state, value).
        if (is_array)
            return TRY(serialize_json_array(vm, state, value.as_object()));

        // c. Return ? SerializeJSONObject(state, value).
        return TRY(serialize_json_object(vm, state, value.as_object()));
    }

    // 12. Return undefined.
    return Optional<String> {};
}

// 25.5.2.4 SerializeJSONObject ( state, value ), https://tc39.es/ecma262/#sec-serializejsonobject
ThrowCompletionOr<String> JSONObject::serialize_json_object(VM& vm, StringifyState& state, Object& object)
{
    if (state.seen_objects.contains(&object))
        return vm.throw_completion<TypeError>(ErrorType::JsonCircular);

    state.seen_objects.set(&object);
    String previous_indent = state.indent;
    state.indent = MUST(String::formatted("{}{}", state.indent, state.gap));
    Vector<String> property_strings;

    auto process_property = [&](PropertyKey const& key) -> ThrowCompletionOr<void> {
        if (key.is_symbol())
            return {};
        auto serialized_property_string = TRY(serialize_json_property(vm, state, key, &object));
        if (serialized_property_string.has_value()) {
            property_strings.append(MUST(String::formatted(
                "{}:{}{}",
                quote_json_string(key.to_string()),
                state.gap.is_empty() ? "" : " ",
                serialized_property_string)));
        }
        return {};
    };

    if (state.property_list.has_value()) {
        auto property_list = state.property_list.value();
        for (auto& property : property_list)
            TRY(process_property(property));
    } else {
        auto property_list = TRY(object.enumerable_own_property_names(PropertyKind::Key));
        for (auto& property : property_list)
            TRY(process_property(property.as_string().utf8_string()));
    }
    StringBuilder builder;
    if (property_strings.is_empty()) {
        builder.append("{}"sv);
    } else {
        bool first = true;
        builder.append('{');
        if (state.gap.is_empty()) {
            for (auto& property_string : property_strings) {
                if (!first)
                    builder.append(',');
                first = false;
                builder.append(property_string);
            }
        } else {
            builder.append('\n');
            builder.append(state.indent);
            auto separator = MUST(String::formatted(",\n{}", state.indent));
            for (auto& property_string : property_strings) {
                if (!first)
                    builder.append(separator);
                first = false;
                builder.append(property_string);
            }
            builder.append('\n');
            builder.append(previous_indent);
        }
        builder.append('}');
    }

    state.seen_objects.remove(&object);
    state.indent = previous_indent;
    return builder.to_string_without_validation();
}

// 25.5.2.5 SerializeJSONArray ( state, value ), https://tc39.es/ecma262/#sec-serializejsonarray
ThrowCompletionOr<String> JSONObject::serialize_json_array(VM& vm, StringifyState& state, Object& object)
{
    if (state.seen_objects.contains(&object))
        return vm.throw_completion<TypeError>(ErrorType::JsonCircular);

    state.seen_objects.set(&object);
    String previous_indent = state.indent;
    state.indent = MUST(String::formatted("{}{}", state.indent, state.gap));
    Vector<String> property_strings;

    auto length = TRY(length_of_array_like(vm, object));

    // Optimization
    property_strings.ensure_capacity(length);

    for (size_t i = 0; i < length; ++i) {
        auto serialized_property_string = TRY(serialize_json_property(vm, state, i, &object));
        if (!serialized_property_string.has_value()) {
            property_strings.append("null"_string);
        } else {
            property_strings.append(serialized_property_string.release_value());
        }
    }

    StringBuilder builder;
    if (property_strings.is_empty()) {
        builder.append("[]"sv);
    } else {
        if (state.gap.is_empty()) {
            builder.append('[');
            bool first = true;
            for (auto& property_string : property_strings) {
                if (!first)
                    builder.append(',');
                first = false;
                builder.append(property_string);
            }
            builder.append(']');
        } else {
            builder.append("[\n"sv);
            builder.append(state.indent);
            auto separator = MUST(String::formatted(",\n{}", state.indent));
            bool first = true;
            for (auto& property_string : property_strings) {
                if (!first)
                    builder.append(separator);
                first = false;
                builder.append(property_string);
            }
            builder.append('\n');
            builder.append(previous_indent);
            builder.append(']');
        }
    }

    state.seen_objects.remove(&object);
    state.indent = previous_indent;
    return builder.to_string_without_validation();
}

// 25.5.2.2 QuoteJSONString ( value ), https://tc39.es/ecma262/#sec-quotejsonstring
String JSONObject::quote_json_string(String string)
{
    // 1. Let product be the String value consisting solely of the code unit 0x0022 (QUOTATION MARK).
    StringBuilder builder;
    builder.append('"');

    // 2. For each code point C of StringToCodePoints(value), do
    auto utf_view = Utf8View(string);
    for (auto code_point : utf_view) {
        // a. If C is listed in the “Code Point” column of Table 70, then
        // i. Set product to the string-concatenation of product and the escape sequence for C as specified in the “Escape Sequence” column of the corresponding row.
        switch (code_point) {
        case '\b':
            builder.append("\\b"sv);
            break;
        case '\t':
            builder.append("\\t"sv);
            break;
        case '\n':
            builder.append("\\n"sv);
            break;
        case '\f':
            builder.append("\\f"sv);
            break;
        case '\r':
            builder.append("\\r"sv);
            break;
        case '"':
            builder.append("\\\""sv);
            break;
        case '\\':
            builder.append("\\\\"sv);
            break;
        default:
            // b. Else if C has a numeric value less than 0x0020 (SPACE), or if C has the same numeric value as a leading surrogate or trailing surrogate, then
            if (code_point < 0x20 || is_unicode_surrogate(code_point)) {
                // i. Let unit be the code unit whose numeric value is that of C.
                // ii. Set product to the string-concatenation of product and UnicodeEscape(unit).
                builder.appendff("\\u{:04x}", code_point);
            }
            // c. Else,
            else {
                // i. Set product to the string-concatenation of product and UTF16EncodeCodePoint(C).
                builder.append_code_point(code_point);
            }
        }
    }
    // 3. Set product to the string-concatenation of product and the code unit 0x0022 (QUOTATION MARK).
    builder.append('"');

    // 4. Return product.
    return builder.to_string_without_validation();
}

// 25.5.1 JSON.parse ( text [ , reviver ] ), https://tc39.es/ecma262/#sec-json.parse
JS_DEFINE_NATIVE_FUNCTION(JSONObject::parse)
{
    auto& realm = *vm.current_realm();

    auto text = vm.argument(0);
    auto reviver = vm.argument(1);

    // 1. Let jsonString be ? ToString(text).
    auto json_string = TRY(text.to_string(vm));

    // 2. Let unfiltered be ? ParseJSON(jsonString).
    auto unfiltered = TRY(parse_json(vm, json_string));

    // 3. If IsCallable(reviver) is true, then
    if (reviver.is_function()) {
        // a. Let root be OrdinaryObjectCreate(%Object.prototype%).
        auto root = Object::create(realm, realm.intrinsics().object_prototype());

        // b. Let rootName be the empty String.
        String root_name;

        // c. Perform ! CreateDataPropertyOrThrow(root, rootName, unfiltered).
        MUST(root->create_data_property_or_throw(root_name, unfiltered));

        // d. Return ? InternalizeJSONProperty(root, rootName, reviver).
        return internalize_json_property(vm, root, root_name, reviver.as_function());
    }
    // 4. Else,
    //     a. Return unfiltered.
    return unfiltered;
}

// 25.5.1.1 ParseJSON ( text ), https://tc39.es/ecma262/#sec-ParseJSON
ThrowCompletionOr<Value> JSONObject::parse_json(VM& vm, StringView text)
{
    auto json = JsonValue::from_string(text);

    // 1. If StringToCodePoints(text) is not a valid JSON text as specified in ECMA-404, throw a SyntaxError exception.
    if (json.is_error())
        return vm.throw_completion<SyntaxError>(ErrorType::JsonMalformed);

    // 2. Let scriptString be the string-concatenation of "(", text, and ");".
    // 3. Let script be ParseText(scriptString, Script).
    // 4. NOTE: The early error rules defined in 13.2.5.1 have special handling for the above invocation of ParseText.
    // 5. Assert: script is a Parse Node.
    // 6. Let result be ! Evaluation of script.
    auto result = JSONObject::parse_json_value(vm, json.value());

    // 7. NOTE: The PropertyDefinitionEvaluation semantics defined in 13.2.5.5 have special handling for the above evaluation.
    // 8. Assert: result is either a String, a Number, a Boolean, an Object that is defined by either an ArrayLiteral or an ObjectLiteral, or null.

    // 9. Return result.
    return result;
}

Value JSONObject::parse_json_value(VM& vm, JsonValue const& value)
{
    if (value.is_object())
        return Value(parse_json_object(vm, value.as_object()));
    if (value.is_array())
        return Value(parse_json_array(vm, value.as_array()));
    if (value.is_null())
        return js_null();
    if (auto double_value = value.get_double_with_precision_loss(); double_value.has_value())
        return Value(double_value.value());
    if (value.is_string())
        return PrimitiveString::create(vm, value.as_string());
    if (value.is_bool())
        return Value(value.as_bool());
    VERIFY_NOT_REACHED();
}

Object* JSONObject::parse_json_object(VM& vm, JsonObject const& json_object)
{
    auto& realm = *vm.current_realm();
    auto object = Object::create(realm, realm.intrinsics().object_prototype());
    json_object.for_each_member([&](auto& key, auto& value) {
        object->define_direct_property(key, parse_json_value(vm, value), default_attributes);
    });
    return object;
}

Array* JSONObject::parse_json_array(VM& vm, JsonArray const& json_array)
{
    auto& realm = *vm.current_realm();
    auto array = MUST(Array::create(realm, 0));
    size_t index = 0;
    json_array.for_each([&](auto& value) {
        array->define_direct_property(index++, parse_json_value(vm, value), default_attributes);
    });
    return array;
}

// 25.5.1.1 InternalizeJSONProperty ( holder, name, reviver ), https://tc39.es/ecma262/#sec-internalizejsonproperty
ThrowCompletionOr<Value> JSONObject::internalize_json_property(VM& vm, Object* holder, PropertyKey const& name, FunctionObject& reviver)
{
    auto value = TRY(holder->get(name));
    if (value.is_object()) {
        auto is_array = TRY(value.is_array(vm));

        auto& value_object = value.as_object();
        auto process_property = [&](PropertyKey const& key) -> ThrowCompletionOr<void> {
            auto element = TRY(internalize_json_property(vm, &value_object, key, reviver));
            if (element.is_undefined())
                TRY(value_object.internal_delete(key));
            else
                TRY(value_object.create_data_property(key, element));
            return {};
        };

        if (is_array) {
            auto length = TRY(length_of_array_like(vm, value_object));
            for (size_t i = 0; i < length; ++i)
                TRY(process_property(i));
        } else {
            auto property_list = TRY(value_object.enumerable_own_property_names(Object::PropertyKind::Key));
            for (auto& property_key : property_list)
                TRY(process_property(property_key.as_string().utf8_string()));
        }
    }

    return TRY(call(vm, reviver, holder, PrimitiveString::create(vm, name.to_string()), value));
}

// 1.3 JSON.rawJSON ( text ), https://tc39.es/proposal-json-parse-with-source/#sec-json.rawjson
JS_DEFINE_NATIVE_FUNCTION(JSONObject::raw_json)
{
    auto& realm = *vm.current_realm();

    // 1. Let jsonString be ? ToString(text).
    auto json_string = TRY(vm.argument(0).to_string(vm));

    // 2. Throw a SyntaxError exception if jsonString is the empty String, or if either the first or last code unit of
    //    jsonString is any of 0x0009 (CHARACTER TABULATION), 0x000A (LINE FEED), 0x000D (CARRIAGE RETURN), or
    //    0x0020 (SPACE).
    auto bytes = json_string.bytes_as_string_view();
    if (bytes.is_empty())
        return vm.throw_completion<SyntaxError>(ErrorType::JsonMalformed);

    static constexpr AK::Array invalid_code_points { 0x09, 0x0A, 0x0D, 0x20 };
    auto first_char = bytes[0];
    auto last_char = bytes[bytes.length() - 1];

    if (invalid_code_points.contains_slow(first_char) || invalid_code_points.contains_slow(last_char))
        return vm.throw_completion<SyntaxError>(ErrorType::JsonMalformed);

    // 3. Parse StringToCodePoints(jsonString) as a JSON text as specified in ECMA-404. Throw a SyntaxError exception
    //    if it is not a valid JSON text as defined in that specification, or if its outermost value is an object or
    //    array as defined in that specification.
    auto json = JsonValue::from_string(json_string);
    if (json.is_error())
        return vm.throw_completion<SyntaxError>(ErrorType::JsonMalformed);

    if (json.value().is_object() || json.value().is_array())
        return vm.throw_completion<SyntaxError>(ErrorType::JsonRawJSONNonPrimitive);

    // 4. Let internalSlotsList be « [[IsRawJSON]] ».
    // 5. Let obj be OrdinaryObjectCreate(null, internalSlotsList).
    auto object = RawJSONObject::create(realm, nullptr);

    // 6. Perform ! CreateDataPropertyOrThrow(obj, "rawJSON", jsonString).
    MUST(object->create_data_property_or_throw(vm.names.rawJSON, PrimitiveString::create(vm, json_string)));

    // 7. Perform ! SetIntegrityLevel(obj, frozen).
    MUST(object->set_integrity_level(Object::IntegrityLevel::Frozen));

    // 8. Return obj.
    return object;
}

// 1.1 JSON.isRawJSON ( O ), https://tc39.es/proposal-json-parse-with-source/#sec-json.israwjson
JS_DEFINE_NATIVE_FUNCTION(JSONObject::is_raw_json)
{
    // 1. If Type(O) is Object and O has an [[IsRawJSON]] internal slot, return true.
    if (vm.argument(0).is_object() && is<RawJSONObject>(vm.argument(0).as_object()))
        return Value(true);

    // 2. Return false.
    return Value(false);
}

}
