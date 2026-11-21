#pragma once

#include <QString>
#include <QtTypes>

#include <optional>

struct Logonomic_cli_capability_progress
{
    quint64                completed = 0;
    std::optional<quint64> total;
    QString                note;
};

enum class Logonomic_cli_addon_activation_state
{
    NOT_STARTED,
    STARTING,
    READY,
    TERMINALLY_UNAVAILABLE,
    IDLE_STOPPING,
};
