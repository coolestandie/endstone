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

#include "bedrock/world/containers/managers/enchanting_container_manager_model.h"

#include <cstddef>
#include <optional>
#include <utility>
#include <vector>

#include <gsl/util>

#include "bedrock/world/actor/player/player.h"
#include "bedrock/world/containers/models/container_model.h"
#include "bedrock/world/item/enchanting/enchant_utils.h"
#include "endstone/check.h"
#include "endstone/core/block/block.h"
#include "endstone/core/enchantments/enchantment.h"
#include "endstone/core/inventory/inventory.h"
#include "endstone/core/inventory/item_stack.h"
#include "endstone/core/player.h"
#include "endstone/core/server.h"
#include "endstone/event/enchantment/prepare_item_enchant_event.h"
#include "endstone/runtime/hook.h"

namespace {
class EndstoneContainerModelInventory final : public endstone::core::EndstoneInventoryBase<endstone::Inventory> {
public:
    explicit EndstoneContainerModelInventory(std::shared_ptr<ContainerModel> model) : model_(std::move(model)) {}

    [[nodiscard]] int getSize() const override { return model_->getContainerSize(); }

    [[nodiscard]] std::optional<endstone::ItemStack> getItem(int index) const override
    {
        const auto &item = model_->getItemStack(index);
        if (item.isNull()) {
            return std::nullopt;
        }
        return endstone::core::EndstoneItemStack::fromMinecraft(item);
    }

    void setItem(int index, std::optional<endstone::ItemStack> item) override
    {
        // NOTE: ContainerModel::setItem forwards to Container::setItem(_getContainerOffset() + index), the path BDS
        // itself writes through, so it skips the setItemWithForceBalance() the base implementation uses.
        const auto item_stack =
            item.has_value() ? endstone::core::EndstoneItemStack::toMinecraft(item.value()) : ItemStack::EMPTY_ITEM;
        model_->setItem(index, item_stack);
    }

    [[nodiscard]] std::vector<std::optional<endstone::ItemStack>> getContents() const override
    {
        std::vector<std::optional<endstone::ItemStack>> contents;
        contents.reserve(model_->getItems().size());
        for (const auto &item : model_->getItems()) {
            if (item.isNull()) {
                contents.emplace_back(std::nullopt);
            }
            else {
                contents.emplace_back(endstone::core::EndstoneItemStack::fromMinecraft(item));
            }
        }
        return contents;
    }

    [[nodiscard]] bool isEmpty() const override
    {
        for (const auto &item : model_->getItems()) {
            if (!item.isNull()) {
                return false;
            }
        }
        return true;
    }

private:
    [[nodiscard]] Container &getContainer() const override
    {
        auto *container = model_->_getContainer();
        endstone::Preconditions::checkState(container != nullptr, "The inventory's backing container is unavailable.");
        return *container;
    }

    std::shared_ptr<ContainerModel> model_;
};

bool getOffer(const ItemEnchantOption &option, std::optional<endstone::EnchantmentOffer> &offer)
{
    const auto instances = option.enchants.getAllEnchants();
    if (instances.empty()) {
        offer = std::nullopt;
        return true;
    }
    if (option.cost <= 0) {
        return false;
    }

    endstone::EnchantmentOffer::Enchantments enchants;
    for (const auto &instance : instances) {
        const auto *enchant = Enchant::getEnchant(instance.getEnchantType());
        if (enchant == nullptr) {
            return false;
        }
        const auto *enchantment =
            endstone::Enchantment::get(endstone::EnchantmentId::minecraft(enchant->getStringId().getString()));
        if (enchantment == nullptr || instance.getEnchantLevel() <= 0 ||
            !enchants.emplace(enchantment, instance.getEnchantLevel()).second) {
            return false;
        }
    }
    offer.emplace(std::move(enchants), option.cost);
    return true;
}

bool isSameOffer(const std::optional<endstone::EnchantmentOffer> &lhs,
                 const std::optional<endstone::EnchantmentOffer> &rhs)
{
    if (!lhs || !rhs) {
        return lhs.has_value() == rhs.has_value();
    }
    return lhs->getCost() == rhs->getCost() && lhs->getEnchants() == rhs->getEnchants();
}

bool applyOffer(ItemEnchantOption &option, const endstone::EnchantmentOffer &offer,
                const std::optional<endstone::EnchantmentOffer> &original_offer)
{
    option.cost = offer.getCost();
    if (!original_offer || offer.getEnchants() != original_offer->getEnchants()) {
        ItemEnchants::Enchantments instances;
        for (const auto &[enchantment, level] : offer.getEnchants()) {
            const auto *endstone_enchantment = dynamic_cast<const endstone::core::EndstoneEnchantment *>(enchantment);
            if (endstone_enchantment == nullptr || level <= 0) {
                return false;
            }
            instances[0].emplace_back(endstone_enchantment->getHandle().getEnchantType(), level);
        }
        option.enchants.setEnchantInstances(std::move(instances));
    }
    return true;
}
}  // namespace

void EnchantingContainerManagerModel::recalculateOptions()
{
    const auto &server = endstone::core::EndstoneServer::getInstance();
    if (!server.getEndstonePluginManager().isEventRegistered<endstone::PrepareItemEnchantEvent>()) {
        ENDSTONE_HOOK_CALL_ORIGINAL(&EnchantingContainerManagerModel::recalculateOptions, this);
        return;
    }

    {
        auto callback = std::exchange(options_changed_callback_, {});
        const auto restore_callback =
            gsl::finally([this, &callback] { options_changed_callback_ = std::move(callback); });
        ENDSTONE_HOOK_CALL_ORIGINAL(&EnchantingContainerManagerModel::recalculateOptions, this);
    }

    const auto publish_options = gsl::finally([this] {
        if (options_changed_callback_) {
            options_changed_callback_(*this);
        }
    });

    // NOTE: BDS keys containers_ by FullContainerName::toString(), which maps ContainerEnumName through its own
    // name table rather than the enumerator spelling; the entry for EnchantingInputContainer is the literal below.
    // toString() is declared in container_enum.h but has no signature yet, so the key is spelled out here.
    const auto input_container_it = containers_.find("enchanting_input_items");
    if (input_container_it == containers_.end() || input_container_it->second == nullptr) {
        return;
    }
    const auto &input_container = input_container_it->second;
    const auto &input = input_container->getItemStack(0);

    endstone::PrepareItemEnchantEvent::Offers offers;
    if (input.isNull() || input.getCount() <= 0 || enchant_options_.size() != offers.size()) {
        return;
    }

    for (std::size_t i = 0; i < offers.size(); ++i) {
        if (!getOffer(enchant_options_[i], offers[i])) {
            return;
        }
    }
    const auto original_offers = offers;

    EndstoneContainerModelInventory inventory{input_container};
    endstone::PrepareItemEnchantEvent event{
        inventory,
        player_.getEndstoneActor<endstone::core::EndstonePlayer>(),
        endstone::core::EndstoneBlock::at(player_.getDimensionBlockSource(), block_pos_),
        endstone::core::EndstoneItemStack::fromMinecraft(input),
        std::move(offers),
        static_cast<int>(
            EnchantUtils::getBookCasePositions(player_.getDimensionBlockSource(), static_cast<Vec3>(block_pos_))
                .size()),
    };
    server.getPluginManager().callEvent(event);

    if (event.isCancelled()) {
        enchant_options_.clear();
        return;
    }

    auto updated_options = enchant_options_;
    auto changed = false;
    for (std::size_t i = 0; i < event.getOffers().size(); ++i) {
        const auto &offer = event.getOffers()[i];
        if (isSameOffer(offer, original_offers[i])) {
            continue;
        }
        if (!offer) {
            // NOTE: recalculateOptions publishes all three options or none at all, so Bedrock has no shape for a
            // single removed offer; dropping one drops them all.
            enchant_options_.clear();
            return;
        }
        if (!applyOffer(updated_options[i], *offer, original_offers[i])) {
            return;
        }
        changed = true;
    }
    if (changed) {
        enchant_options_ = std::move(updated_options);
    }
}
