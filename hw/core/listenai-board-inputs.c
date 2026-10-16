/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Validate one complete batch before applying electrical input changes. */
#include "qemu/osdep.h"
#include "hw/core/listenai-board-inputs.h"
#include "hw/i2c/listenai_extio.h"
#include "qobject/qjson.h"
#include "qobject/qnum.h"
#include "qobject/qbool.h"
#include "qapi/error.h"
#include "qemu/cutils.h"

bool listenai_board_inputs_parse(ListenaiBoard *b, ListenaiBoardInputs *state,
                                const char *value, unsigned pa, unsigned pb,
                                unsigned adc_first, unsigned adc_end, Error **errp)
{
    if (strlen(value) > 8192) { error_setg(errp, "Input payload too large"); return false; }
    QObject *root = qobject_from_json(value, errp);
    if (!root) { return false; }
    ListenaiBoardInputs next = { .initialized = true };
    QDict *dict = qobject_to(QDict, root);
    QDict *pads = dict ? qdict_get_qdict(dict, "pads") : NULL;
    QDict *adc = dict ? qdict_get_qdict(dict, "adc_uv") : NULL;
    QDict *devices = dict ? qdict_get_qdict(dict, "device_adc_uv") : NULL;
    if (!dict || qdict_size(dict) != (devices ? 3 : 2) || !pads || !adc) { goto invalid; }
    for (const QDictEntry *e = qdict_first(pads); e; e = qdict_next(pads, e)) {
        uint64_t pin;
        QBool *level = qobject_to(QBool, qdict_entry_value(e));
        if (qemu_strtou64(qdict_entry_key(e), NULL, 10, &pin) || pin >= 96 || !level) { goto invalid; }
        if (pin < 64) {
            if ((pin < 32 && pin >= pa) || (pin >= 32 && pin >= 32 + pb)) { goto invalid; }
            next.pad_mask |= UINT64_C(1) << pin;
            if (qbool_get_bool(level)) { next.pad_levels |= UINT64_C(1) << pin; }
        } else {
            if (!b->extio) { goto invalid; }
            pin -= 64;
            static const unsigned valid[] = {6, 0, 0x9f, 0x7f};
            if (!(valid[pin / 8] & (1u << (pin % 8)))) { goto invalid; }
            next.extio_mask |= 1u << pin;
            if (qbool_get_bool(level)) { next.extio_levels |= 1u << pin; }
        }
    }
    for (const QDictEntry *e = qdict_first(adc); e; e = qdict_next(adc, e)) {
        uint64_t channel, uv;
        QNum *n = qobject_to(QNum, qdict_entry_value(e));
        if (qemu_strtou64(qdict_entry_key(e), NULL, 10, &channel) ||
            channel < adc_first || channel >= adc_end || channel >= 16 ||
            !n || !qnum_get_try_uint(n, &uv) || uv > 3600000) { goto invalid; }
        next.adc_mask |= 1u << channel; next.adc_uv[channel] = uv;
    }
    if (devices && qdict_size(devices)) {
        QDict *values = b->extio_id ? qdict_get_qdict(devices, b->extio_id) : NULL;
        if (!b->extio || qdict_size(devices) != 1 || !values) { goto invalid; }
        for (const QDictEntry *e = qdict_first(values); e; e = qdict_next(values, e)) {
            uint64_t channel, uv;
            QNum *n = qobject_to(QNum, qdict_entry_value(e));
            if (qemu_strtou64(qdict_entry_key(e), NULL, 10, &channel) || channel >= 8 ||
                !n || !qnum_get_try_uint(n, &uv) || uv > 3300000) { goto invalid; }
            next.extio_adc_mask |= 1u << channel; next.extio_adc_uv[channel] = uv;
        }
    }
    if (state->initialized && (state->pad_mask != next.pad_mask || state->extio_mask != next.extio_mask ||
        state->adc_mask != next.adc_mask || state->extio_adc_mask != next.extio_adc_mask)) { goto invalid; }
    *state = next;
    qobject_unref(root);
    return true;
invalid:
    qobject_unref(root);
    error_setg(errp, "Invalid board input levels or wiring");
    return false;
}

void listenai_board_inputs_apply(ListenaiBoard *b, ListenaiBoardInputs *state, DeviceState *soc)
{
    for (unsigned i = 0; i < 64; i++) {
        if (state->pad_mask & (UINT64_C(1) << i)) {
            listenai_board_gpio_input(b, soc, i, !!(state->pad_levels & (UINT64_C(1) << i)));
        }
    }
    for (unsigned i = 0; i < 32; i++) {
        if (state->extio_mask & (1u << i)) {
            listenai_board_gpio_input(b, soc, 64 + i, !!(state->extio_levels & (1u << i)));
        }
    }
    for (unsigned i = 0; i < 8; i++) {
        if (state->extio_adc_mask & (1u << i)) {
            bool ok = listenai_extio_voltage(b->extio, i, state->extio_adc_uv[i]);
            assert(ok);
        }
    }
}
