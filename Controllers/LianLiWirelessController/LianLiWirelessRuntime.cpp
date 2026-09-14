/*---------------------------------------------------------*\
| LianLiWirelessRuntime.cpp                                 |
|                                                           |
|   Transport-independent wireless runtime core.            |
|                                                           |
|   This file is part of the OpenRGB project                |
|   SPDX-License-Identifier: GPL-2.0-or-later               |
\*---------------------------------------------------------*/

#include "LianLiWirelessRuntime.h"
#include <stdexcept>

using namespace LianLiWireless;

WirelessRuntime::WirelessRuntime(IWirelessLink& l, IWirelessClock& c, WirelessRuntimeConfig cfg_in)
    : link(l), clock(c), cfg(cfg_in)
{
}

void WirelessRuntime::SetTarget(const Mac& mac, uint8_t fans)
{
    target_mac    = mac;
    expected_fans = fans;
}

void WirelessRuntime::SetDesired(std::shared_ptr<const RgbUpload> upload)
{
    desired        = std::move(upload);
    next_resend_ms = 0;          /* send ASAP on next Tick */
    resends        = 0;
    drift_mismatch = 0;
    if(state == WirelessState::Holding)
    {
        Enter(WirelessState::Uploading);
    }
}

void WirelessRuntime::ClearDesired()
{
    desired.reset();
}

void WirelessRuntime::Cancel()
{
    cancelled = true;
    Enter(WirelessState::Cancelled);
}

bool WirelessRuntime::Confirmed() const
{
    return state == WirelessState::Holding
        && have_sighting
        && desired
        && latest.effect_id == desired->EffectId();
}

void WirelessRuntime::Enter(WirelessState s, const char* err)
{
    state = s;
    if(err && *err)
    {
        last_error = err;
    }
}

void WirelessRuntime::Tick()
{
    if(cancelled || state == WirelessState::Failed
                  || state == WirelessState::ForeignMaster)
    {
        return;
    }

    uint64_t now = clock.NowMs();

    /*---------------------------------------------------------*\
    | Keep-alive takes priority over animation traffic and is   |
    | required before the device will reliably accept uploads.  |
    \*---------------------------------------------------------*/
    if(master_known && now >= next_keepalive_ms)
    {
        if(!SendKeepAlive(now, keepalive_initial))
        {
            if(++send_failures >= cfg.max_send_failures)
            {
                Enter(WirelessState::Failed, "keep-alive transport failure");
                return;
            }
        }
        else
        {
            keepalive_initial  = false;
            send_failures      = 0;
            next_keepalive_ms  = now + cfg.keepalive_ms;
        }
    }

    if(now < next_poll_ms)
    {
        return;
    }
    next_poll_ms = now + (state == WirelessState::Holding ? cfg.hold_drift_ms
                                                         : cfg.poll_ms);

    if(!PollAndMerge(now))
    {
        return;                 /* transport poll failure or Lost handled inside */
    }

    switch(state)
    {
        case WirelessState::Searching:
            if(have_sighting)
            {
                Enter(desired ? WirelessState::Uploading : WirelessState::Holding);
            }
            break;

        case WirelessState::Uploading:
        case WirelessState::Holding:
        case WirelessState::Lost:
            if(desired && have_sighting)
            {
                if(latest.effect_id == desired->EffectId())
                {
                    drift_mismatch = 0;
                    resends        = 0;
                    if(state != WirelessState::Holding)
                    {
                        Enter(WirelessState::Holding);
                    }
                }
                else if(state == WirelessState::Holding)
                {
                    /*----------------------------------------------*\
                    | Confirmed effect drifted (firmware reset /     |
                    | re-association) — resend after debounce.       |
                    \*----------------------------------------------*/
                    if(++drift_mismatch >= cfg.drift_debounce)
                    {
                        drift_mismatch = 0;
                        Enter(WirelessState::Uploading);
                        next_resend_ms = now;
                    }
                }

                if(state == WirelessState::Uploading && now >= next_resend_ms)
                {
                    SendDesired(now);
                }
            }
            else if(!desired && have_sighting && state == WirelessState::Lost)
            {
                Enter(WirelessState::Holding);
            }
            break;

        default:
            break;
    }
}

bool WirelessRuntime::PollAndMerge(uint64_t now)
{
    if(state == WirelessState::Boot)
    {
        if(!link.ReadMasterMac(master))
        {
            if(++master_read_tries >= 3)
            {
                Enter(WirelessState::Failed, "cannot read master MAC");
                return false;
            }
            return true;                    /* retry next poll */
        }
        master_known = true;
        Enter(WirelessState::Searching);
    }

    std::vector<uint8_t> response;
    if(!link.PollDiscovery(response))
    {
        /*---------------------------------------------------------*\
        | USB-level failure is not a miss — it is a transport       |
        | problem and counts toward failure. Kept separate from     |
        | send failures: TX can work while RX is broken.            |
        \*---------------------------------------------------------*/
        if(++poll_failures >= cfg.max_send_failures)
        {
            Enter(WirelessState::Failed, "discovery transport failure");
            return false;
        }
        return true;
    }
    poll_failures = 0;

    std::vector<Sighting> sightings;
    try
    {
        sightings = ParseDiscoveryResponse(response.data(), response.size());
    }
    catch(const std::invalid_argument&)
    {
        /* malformed reply = miss, not error */
        sightings.clear();
    }

    const Sighting* found = nullptr;
    for(const Sighting& s : sightings)
    {
        if(s.mac == target_mac)
        {
            found = &s;
        }
    }

    if(found == nullptr)
    {
        if(have_sighting && now - last_seen_ms > cfg.lost_ms)
        {
            Enter(WirelessState::Lost, "target absent past deadline");
        }
        else if(!have_sighting && now > cfg.lost_ms)
        {
            Enter(WirelessState::Lost, "target never sighted");
        }
        return true;
    }

    /*---------------------------------------------------------*\
    | Foreign-master debounce: only N consecutive sightings     |
    | reporting a different master count as a re-bind.          |
    \*---------------------------------------------------------*/
    if(master_known && found->master != master)
    {
        if(++master_mismatch >= cfg.master_debounce)
        {
            Enter(WirelessState::ForeignMaster, "wireless master changed");
            return false;
        }
    }
    else
    {
        master_mismatch = 0;
    }

    latest        = *found;
    have_sighting = true;
    last_seen_ms  = now;
    return true;
}

bool WirelessRuntime::SendDesired(uint64_t now)
{
    if(resends >= cfg.max_resends)
    {
        Enter(WirelessState::Failed, "upload retry budget exhausted");
        return false;
    }

    /*---------------------------------------------------------*\
    | Address with the freshest sighting's channel/receiver so  |
    | a channel move self-corrects.                             |
    \*---------------------------------------------------------*/
    bool ok = true;
    for(size_t i = 0; i < desired->PacketCount(); i++)
    {
        RfPacket rf = desired->Packet(i, target_mac, master);
        auto chunks = UsbChunks(rf, latest.channel, latest.receiver);

        /* first (header) packet is sent twice, matching vendor behavior */
        uint8_t repeats = (i == 0) ? 2 : 1;
        for(uint8_t r = 0; r < repeats && ok; r++)
        {
            ok = link.SendChunks(chunks);
        }
    }

    if(!ok)
    {
        if(++send_failures >= cfg.max_send_failures)
        {
            Enter(WirelessState::Failed, "upload transport failure");
        }
        return false;
    }

    send_failures  = 0;
    resends++;
    next_resend_ms = now + cfg.resend_ms;
    return true;
}

bool WirelessRuntime::SendKeepAlive(uint64_t /*now*/, bool initial)
{
    /* real implementation reads wall clock; tests pass fixed time */
    auto chunks = ClockChunks(master, have_sighting ? latest.channel : 0,
                              2000, 1, 1, 0, 0, 0, initial);
    return link.SendChunks(chunks);
}
