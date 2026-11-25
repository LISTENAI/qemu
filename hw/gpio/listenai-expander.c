/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "hw/gpio/listenai-expander.h"
#include "hw/i2c/listenai_extio.h"
#include "hw/i2c/gxa574.h"

static bool quasi(DeviceState *dev)
{
    if (object_dynamic_cast(OBJECT(dev), TYPE_GXA574)) { return true; }
    assert(object_dynamic_cast(OBJECT(dev), TYPE_LISTENAI_EXTIO));
    return false;
}

void listenai_expander_outputs(DeviceState *dev, unsigned bank,
                              uint32_t *driven, uint32_t *levels)
{
    assert(bank < 4);
    if (quasi(dev)) {
        *driven = *levels = 0;
        if (!bank) { gxa574_outputs(dev, driven, levels); }
    } else {
        listenai_extio_outputs(dev, bank, driven, levels);
    }
}

bool listenai_expander_active(DeviceState *dev, unsigned pin, bool level)
{
    uint32_t driven, levels;
    listenai_expander_outputs(dev, pin / 8, &driven, &levels);
    uint32_t mask = 1u << (pin % 8);
    return (quasi(dev) || (driven & mask)) && !!(levels & mask) == level;
}

double listenai_expander_duty(DeviceState *dev, unsigned pin)
{
    if (!quasi(dev)) { return listenai_extio_duty(dev, pin); }
    assert(pin < 8);
    uint32_t driven, levels;
    gxa574_outputs(dev, &driven, &levels);
    /* A released quasi-bidirectional pin still has its weak pull-up. */
    return !!(levels & (1u << pin));
}

uint32_t listenai_expander_valid_pins(DeviceState *dev)
{
    return quasi(dev) ? gxa574_valid_pins() : listenai_extio_valid_pins();
}

unsigned listenai_expander_adc_channels(DeviceState *dev)
{
    return quasi(dev) ? 0 : 8;
}

bool listenai_expander_voltage(DeviceState *dev, unsigned channel, unsigned uv)
{
    return !quasi(dev) && listenai_extio_voltage(dev, channel, uv);
}

void listenai_expander_observe(DeviceState *dev,
                              void (*observe)(void *, unsigned, uint32_t, uint32_t),
                              void *opaque)
{
    if (quasi(dev)) { gxa574_observe(dev, observe, opaque); }
    else { listenai_extio_observe(dev, observe, opaque); }
}
