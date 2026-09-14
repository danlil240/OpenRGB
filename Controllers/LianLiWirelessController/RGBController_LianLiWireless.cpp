/*---------------------------------------------------------*\
|| RGBController_LianLiWireless.cpp                          ||
||                                                           ||
||   RGBController for one Lian Li wireless fan group.       ||
||   The firmware receives full per-LED frames rather than   ||
||   hardware effect commands, so every mode renders to a    ||
||   compressed frame upload.                                ||
||                                                           ||
||   This file is part of the OpenRGB project                ||
||   SPDX-License-Identifier: GPL-2.0-or-later               ||
\*---------------------------------------------------------*/

#include "RGBController_LianLiWireless.h"
#include <string>
#include <vector>

/**------------------------------------------------------------------*\
    @name Lian Li Wireless Fan Group
    @type USB
    @save :x:
    @direct :rotating_light:
    @effects :x:
    @detectors DetectLianLiWireless
    @comment Fan groups are discovered over the wireless RX dongle at
             detect time. Groups that appear later (or while L-Connect
             holds the receiver) need an app restart/rescan with the
             dongles free.
\*-------------------------------------------------------------------*/

enum
{
    LLW_MODE_DIRECT = 0,
    LLW_MODE_STATIC = 1,
};

RGBController_LianLiWireless::RGBController_LianLiWireless(LianLiWirelessController* controller_ptr)
{
    controller  = controller_ptr;

    name        = controller->GetFanName() + " " + controller->GetMacString().substr(12);
    vendor      = "Lian Li";
    type        = DEVICE_TYPE_COOLER;
    description = "Lian Li Wireless Fan Group";
    location    = "Wireless: " + controller->GetMacString();
    serial      = controller->GetMacString();

    mode Direct;
    Direct.name                 = "Direct";
    Direct.value                = LLW_MODE_DIRECT;
    Direct.flags                = MODE_FLAG_HAS_PER_LED_COLOR;
    Direct.color_mode           = MODE_COLORS_PER_LED;
    modes.push_back(Direct);

    mode Static;
    Static.name                 = "Static";
    Static.value                = LLW_MODE_STATIC;
    Static.flags                = MODE_FLAG_HAS_MODE_SPECIFIC_COLOR;
    Static.colors_min           = 1;
    Static.colors_max           = 1;
    Static.color_mode           = MODE_COLORS_MODE_SPECIFIC;
    Static.colors.resize(1);
    modes.push_back(Static);

    SetupZones();
}

RGBController_LianLiWireless::~RGBController_LianLiWireless()
{
    Shutdown();

    delete controller;
}

void RGBController_LianLiWireless::SetupZones()
{
    /*-------------------------------------------------*\
    | One linear zone per fan — the frame payload is    |
    | fan-major: fan i owns LEDs [i*leds_per_fan,       |
    | (i+1)*leds_per_fan).                              |
    \*-------------------------------------------------*/
    leds.clear();
    colors.clear();
    zones.resize(controller->GetFanCount());

    for(std::size_t fan_idx = 0; fan_idx < zones.size(); fan_idx++)
    {
        zone& z = zones[fan_idx];

        z.name          = "Fan " + std::to_string(fan_idx + 1);
        z.type          = ZONE_TYPE_LINEAR;
        z.leds_min      = controller->GetLEDsPerFan();
        z.leds_max      = controller->GetLEDsPerFan();
        z.leds_count    = controller->GetLEDsPerFan();

        for(unsigned int led_idx = 0; led_idx < z.leds_count; led_idx++)
        {
            led new_led;
            new_led.name    = z.name + ", LED " + std::to_string(led_idx + 1);
            new_led.value   = (unsigned int)fan_idx;
            leds.push_back(new_led);
        }
    }

    SetupColors();
}

void RGBController_LianLiWireless::DeviceConfigureZone(int /* zone_idx */)
{
    SetupZones();
}

void RGBController_LianLiWireless::DeviceUpdateLEDs()
{
    controller->SetLEDs(colors.data(), colors.size());
}

void RGBController_LianLiWireless::DeviceUpdateZoneLEDs(int /* zone */)
{
    /*-------------------------------------------------*\
    | The firmware takes whole-group frames — updating  |
    | one zone still uploads the full color buffer.     |
    \*-------------------------------------------------*/
    DeviceUpdateLEDs();
}

void RGBController_LianLiWireless::DeviceUpdateSingleLED(int /* led */)
{
    DeviceUpdateLEDs();
}

void RGBController_LianLiWireless::DeviceUpdateMode()
{
    if(modes[active_mode].value == LLW_MODE_STATIC)
    {
        std::vector<RGBColor> solid(colors.size(), modes[active_mode].colors[0]);
        controller->SetLEDs(solid.data(), solid.size());
    }
    else
    {
        DeviceUpdateLEDs();
    }
}
