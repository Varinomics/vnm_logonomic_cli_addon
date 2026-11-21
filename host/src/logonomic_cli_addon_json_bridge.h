#pragma once

#include "logonomic_cli_addon/json.h"

#include <QJsonObject>
#include <QJsonValue>
#include <QString>

#include <optional>

// Conversion between the SDK's strict JSON document model and Qt's. The SDK
// codec is the single parser and validator on the host: bytes that cross the
// adapter boundary are parsed once, by it, and the validated document is
// converted here for the Qt-facing broker API. Nothing here parses or
// serializes text.
//
// Integers travel as exact std::int64_t on the SDK side and qint64 on the Qt
// side. QJsonValue exposes integers and floating-point numbers under the same
// Double type; its QVariant conversion distinguishes them without routing
// exact integers through double and losing precision.

QJsonValue logonomic_cli_addon_to_qjson(const logonomic::cli_addon::Json_value& value);
logonomic::cli_addon::Json_value logonomic_cli_addon_from_qjson(const QJsonValue& value);

// Only the SDK's validated value-free diagnostic reaches a log. A recognized
// but invalid envelope returns a marker so its raw message stays out of the UI.
std::optional<QJsonObject> logonomic_cli_addon_event_diagnostic(const QString& message);
