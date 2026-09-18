// SPDX-License-Identifier: GPL-3.0-only
/*
 * Copyright (C) 2022-present, PenUniverse.
 * This file is part of the PenMods open source project.
 */

#include "torch/Torch.h"

#include "common/Event.h"
#include "common/Utils.h"

#include <QQmlContext>

#if PL_BUILD_YDP02X
constexpr auto LED_DEFAULT_GPIO_ID = 15;
#else
// 三代（YDP03X）LED 控制由 librkdev.so 提供：led_init(int gpio) 后接无参 led_on()/led_off()。
namespace {
constexpr int LED_DEFAULT_GPIO_ID = 15;
using LedInitFn  = void (*)(int);
using LedOnOffFn = void (*)();
} // namespace
#endif

namespace mod {

Torch::Torch() {
    connect(&Event::getInstance(), &Event::beforeUiInitialization, [this](QQuickView& view, QQmlContext* context) {
        context->setContextProperty("torch", this);
    });
}

bool Torch::getStatus() { return exec(QString("cat /sys/class/gpio/gpio%1/value").arg(LED_DEFAULT_GPIO_ID)) == "1"; }

void Torch::setStatus(bool stat) {
    if (getStatus() != stat) {
        if (stat) {
#if PL_BUILD_YDP02X
            PEN_CALL(void*, "led_on", uint32)(LED_DEFAULT_GPIO_ID);
#else
            auto init = PEN_CALL(LedInitFn, "led_init", int);
            if (init) {
                init(LED_DEFAULT_GPIO_ID);
            }
            auto on = PEN_CALL(LedOnOffFn, "led_on");
            if (on) {
                on();
            }
#endif
        } else {
#if PL_BUILD_YDP02X
            PEN_CALL(void*, "led_off", uint32)(LED_DEFAULT_GPIO_ID);
#else
            auto off = PEN_CALL(LedOnOffFn, "led_off");
            if (off) {
                off();
            }
#endif
        }
        emit statusChanged();
    }
}

} // namespace mod
