#include "activation_policy.hpp"
#include <utility>

namespace qs::windows {

namespace {
thread_local std::optional<ActivationSource> gActivationSource; // NOLINT
}

ActivationDispatchScope::ActivationDispatchScope(ActivationSource source)
    : previous(std::exchange(gActivationSource, source)) {}

ActivationDispatchScope::~ActivationDispatchScope() { gActivationSource = this->previous; }

std::optional<ActivationSource> currentActivationSource() { return gActivationSource; }

} // namespace qs::windows
