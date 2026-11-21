#include "logonomic_cli_addon_json_bridge.h"

#include "logonomic_cli_addon/provider_event_diagnostic.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QMetaType>
#include <QString>
#include <QVariant>

#include <cstdint>
#include <string>

namespace {

namespace sdk = logonomic::cli_addon;

} // namespace

QJsonValue logonomic_cli_addon_to_qjson(const sdk::Json_value& value)
{
    switch (value.type()) {
        case sdk::Json_type::NULL_VALUE: return QJsonValue(QJsonValue::Null);
        case sdk::Json_type::BOOLEAN:    return QJsonValue(value.as_boolean());
        case sdk::Json_type::INTEGER:    return QJsonValue(static_cast<qint64>(value.as_integer()));
        case sdk::Json_type::NUMBER:     return QJsonValue(value.as_number());
        case sdk::Json_type::STRING:     return QJsonValue(QString::fromStdString(value.as_string()));
        case sdk::Json_type::ARRAY: {
            QJsonArray array;
            for (const sdk::Json_value& item : value.as_array()) {
                array.append(logonomic_cli_addon_to_qjson(item));
            }
            return array;
        }
        case sdk::Json_type::OBJECT:
        default: {
            QJsonObject object;
            for (const sdk::Json_member& member : value.as_object()) {
                object.insert(
                    QString::fromStdString(member.name),
                    logonomic_cli_addon_to_qjson(member.value));
            }
            return object;
        }
    }
}

sdk::Json_value logonomic_cli_addon_from_qjson(const QJsonValue& value)
{
    switch (value.type()) {
        case QJsonValue::Bool:
            return sdk::Json_value(value.toBool());
        case QJsonValue::Double: {
            const QVariant number = value.toVariant();
            if (number.metaType().id() == QMetaType::LongLong) {
                return sdk::Json_value(static_cast<std::int64_t>(number.toLongLong()));
            }
            return sdk::Json_value(number.toDouble());
        }
        case QJsonValue::String:
            return sdk::Json_value(value.toString().toStdString());
        case QJsonValue::Array: {
            sdk::Json_value array = sdk::Json_value::array();
            for (const QJsonValue& item : value.toArray()) {
                array.push_back(logonomic_cli_addon_from_qjson(item));
            }
            return array;
        }
        case QJsonValue::Object: {
            sdk::Json_value object = sdk::Json_value::object();
            const QJsonObject source = value.toObject();
            for (auto it = source.constBegin(); it != source.constEnd(); ++it) {
                object.set(it.key().toStdString(), logonomic_cli_addon_from_qjson(it.value()));
            }
            return object;
        }
        case QJsonValue::Null:
        case QJsonValue::Undefined:
        default:
            return sdk::Json_value();
    }
}

std::optional<QJsonObject> logonomic_cli_addon_event_diagnostic(const QString& message)
{
    const std::string bytes = message.toStdString();
    const auto parsed = sdk::parse_json(bytes, sdk::Json_parse_limits{}, nullptr);
    if (!parsed || !parsed->is_object() || !parsed->find(sdk::k_provider_event_diagnostic_member)) {
        if (bytes.find(sdk::k_provider_event_diagnostic_member) != std::string::npos) {
            return QJsonObject{{QStringLiteral("diagnostic_invalid"), true}};
        }
        return std::nullopt;
    }
    const auto diagnostic = sdk::decode_provider_event_diagnostic(bytes);
    if (!diagnostic) {
        return QJsonObject{{QStringLiteral("diagnostic_invalid"), true}};
    }
    const auto canonical = sdk::parse_json(
        sdk::encode_provider_event_diagnostic(*diagnostic), sdk::Json_parse_limits{}, nullptr);
    return logonomic_cli_addon_to_qjson(*canonical).toObject();
}
