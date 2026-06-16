#include <haicode/provider.h>

namespace haicode {

void ProviderRegistry::register_provider(std::shared_ptr<Provider> provider) {
    providers_[provider->id()] = std::move(provider);
}

std::shared_ptr<Provider> ProviderRegistry::get(const std::string& provider_id) const {
    auto it = providers_.find(provider_id);
    if (it == providers_.end()) return nullptr;
    return it->second;
}

std::vector<std::string> ProviderRegistry::available_ids() const {
    std::vector<std::string> ids;
    for (auto& [k, _] : providers_)
        ids.push_back(k);
    return ids;
}

} // namespace haicode
