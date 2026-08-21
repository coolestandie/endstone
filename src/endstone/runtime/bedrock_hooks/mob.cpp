// Copyright (c) 2024, The Endstone Project. (https://endstone.dev) All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "bedrock/world/actor/mob.h"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>

#include "bedrock/entity/components/damage_sensor_component.h"
#include "bedrock/entity/components/no_action_time_component.h"
#include "endstone/actor/actor.h"
#include "endstone/actor/mob.h"
#include "endstone/core/actor/mob.h"
#include "endstone/core/damage/damage_source.h"
#include "endstone/core/entity/components/flag_components.h"
#include "endstone/core/json.h"
#include "endstone/core/server.h"
#include "endstone/event/actor/actor_damage_event.h"
#include "endstone/event/actor/actor_knockback_event.h"
#include "endstone/runtime/hook.h"

namespace {
struct KnockbackConfig {
    bool enabled = false;
    bool players_only = true;
    bool debug = false;
    float friction = 2.0F;
    float horizontal_ground = 0.4F;
    float horizontal_air = 0.4F;
    float vertical_ground = 0.4F;
    float vertical_air = 0.4F;
    float vertical_limit = 0.4F;
    float extra_horizontal = 0.5F;
    float extra_vertical = 0.1F;
    int hit_delay = -1;
};

KnockbackConfig loadKnockbackConfig(const std::filesystem::path &path)
{
    KnockbackConfig config;
    std::ifstream file(path);
    if (!file) {
        return config;
    }
    try {
        const auto json = nlohmann::json::parse(file);
        config.enabled = json.value("enabled", false);
        config.players_only = json.value("players_only", true);
        if (const auto it = json.find("debug"); it != json.end()) {
            config.debug = it->is_boolean() ? it->get<bool>() : (it->is_number() && it->get<double>() != 0.0);
        }
        config.friction = json.value("friction", 2.0F);
        if (const auto it = json.find("horizontal"); it != json.end()) {
            config.horizontal_ground = it->value("ground", 0.4F);
            config.horizontal_air = it->value("air", 0.4F);
        }
        if (const auto it = json.find("vertical"); it != json.end()) {
            config.vertical_ground = it->value("ground", 0.4F);
            config.vertical_air = it->value("air", 0.4F);
        }
        config.vertical_limit = json.value("vertical_limit", 0.4F);
        config.extra_horizontal = json.value("extra_horizontal", 0.5F);
        config.extra_vertical = json.value("extra_vertical", 0.1F);
        config.hit_delay = static_cast<int>(json.value("hit_delay", -1.0));
    }
    catch (const std::exception &) {
        return KnockbackConfig{};
    }
    return config;
}

const KnockbackConfig &getKnockbackConfig()
{
    static KnockbackConfig config;
    static std::filesystem::file_time_type last_write;
    static const std::filesystem::path path{"knockback.json"};
    std::error_code ec;
    const auto write_time = std::filesystem::last_write_time(path, ec);
    if (ec) {
        static const KnockbackConfig disabled;
        return disabled;
    }
    if (write_time != last_write) {
        last_write = write_time;
        config = loadKnockbackConfig(path);
    }
    return config;
}
}  // namespace

void Mob::knockback(Actor *source, float damage, float dx, float dz, const KnockbackParameters &parameters)
{
    const auto &config = getKnockbackConfig();
    const auto before = getPosDelta();
    Vec3 diff = Vec3::ZERO;
    const bool engine = config.enabled && (!config.players_only || isPlayer());

    if (!engine) {
        ENDSTONE_HOOK_CALL_ORIGINAL(&Mob::knockback, this, source, damage, dx, dz, parameters);
        const auto after = getPosDelta();
        diff = after - before;
    }
    else {
        float dir_x = 0.0F;
        float dir_z = 0.0F;
        if (source != nullptr) {
            dir_x = getPosition().x - source->getPosition().x;
            dir_z = getPosition().z - source->getPosition().z;
        }
        if (std::abs(dir_x) < 1e-4F && std::abs(dir_z) < 1e-4F) {
            dir_x = -dx;
            dir_z = -dz;
        }
        const float length = std::sqrt(dir_x * dir_x + dir_z * dir_z);
        const bool on_ground = isOnGround();
        const float horizontal = on_ground ? config.horizontal_ground : config.horizontal_air;
        const float vertical = on_ground ? config.vertical_ground : config.vertical_air;
        const bool has_extra = parameters.extra_knockback_power > 0.0F;

        Vec3 result = Vec3::ZERO;
        result.x = before.x / config.friction;
        result.y = before.y / config.friction + vertical + (has_extra ? config.extra_vertical : 0.0F);
        result.z = before.z / config.friction;
        if (length > 1e-4F) {
            const float power = horizontal + (has_extra ? config.extra_horizontal : 0.0F);
            result.x += dir_x / length * power;
            result.z += dir_z / length * power;
        }
        if (result.y > config.vertical_limit) {
            result.y = config.vertical_limit;
        }
        diff = result - before;
    }

    const auto &server = endstone::core::EndstoneServer::getInstance();
    if (config.debug) {
        server.getLogger().info(
            "[KBRAW] mode={} victim={} src={} dmg={:.2f} dxz=({:.3f},{:.3f}) power=({:.3f},{:.3f}) cap={:.3f} "
            "slow={:.3f} scale_dmg={} legacy={} extra={:.3f} approach={} inv={} diff=({:.3f},{:.3f},{:.3f})",
            engine ? "engine" : "vanilla", getEndstoneActor<endstone::core::EndstoneMob>()->getName(),
            source == nullptr ? "none" : source->getEndstoneActor()->getName(), damage, dx, dz, parameters.power.x,
            parameters.power.y, parameters.vertical_velocity_cap, parameters.slowdown_scale,
            parameters.scale_with_damage, parameters.check_legacy_pre_nether_update_knockback,
            parameters.extra_knockback_power, static_cast<int>(parameters.extra_knockback_approach),
            invulnerable_time, diff.x, diff.y, diff.z);
    }
    endstone::ActorKnockbackEvent e{getEndstoneActor<endstone::core::EndstoneMob>(),
                                    source == nullptr ? nullptr : source->getEndstoneActor(),
                                    {diff.x, diff.y, diff.z}};
    server.getPluginManager().callEvent(e);

    const auto knockback = e.getKnockback();
    diff = e.isCancelled() ? Vec3::ZERO : Vec3{knockback.getX(), knockback.getY(), knockback.getZ()};
    setPosDelta(before + diff);

    if (engine && config.hit_delay >= 0 && invulnerable_time > config.hit_delay) {
        invulnerable_time = config.hit_delay;
    }
}

// bool Mob::_hurt(const ActorDamageSource &source, float damage, bool knock, bool ignite)
// {
//     addOrRemoveComponent<endstone::core::MobHurtFlagComponent>(true);
//     auto result = ENDSTONE_HOOK_CALL_ORIGINAL(&Mob::_hurt, this, source, damage, knock, ignite);
//     if (!hasComponent<endstone::core::MobHurtFlagComponent>()) {
//         // A related ActorDamageEvent is triggered and cancelled, propagate the result to the caller to prevent kb
//         // See also: HealthAttributeDelegate::change
//         return false;
//     }
//     addOrRemoveComponent<endstone::core::MobHurtFlagComponent>(false);
//     return result;
// }
