#include "config/parameter_table.hpp"

namespace config {

// Define GEN_DESCRIPTOR macro (positional initialization for C++ compatibility)
#define GEN_DESCRIPTOR(subsys, param, nvs_key, field, type, min, max, reset, desc, unit, validator_tag) \
  { #subsys "." #param, \
    nvs_key, \
    NvsType::type, \
    offsetof(DeviceConfig, field), \
    sizeof(decltype(std::declval<DeviceConfig&>().field)), \
    MakeValidator<decltype(std::declval<DeviceConfig&>().field)>(min, max, validator_tag{}), \
    reset, \
    desc, \
    unit },

// Define the global parameter descriptor array
const ParameterDescriptor kParameterDescriptors[kParameterCount] = {
  PARAMETER_TABLE(GEN_DESCRIPTOR)
};

#undef GEN_DESCRIPTOR

}  // namespace config
