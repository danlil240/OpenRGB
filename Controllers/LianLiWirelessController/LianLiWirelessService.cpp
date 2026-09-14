/*---------------------------------------------------------*\
|| LianLiWirelessService.cpp                                 ||
||                                                           ||
||   This file is part of the OpenRGB project                ||
||   SPDX-License-Identifier: GPL-2.0-or-later               ||
\*---------------------------------------------------------*/

#include "LianLiWirelessService.h"
#include <chrono>

using namespace LianLiWireless;

LianLiWirelessService::LianLiWirelessService(IWirelessLink& link_in,
                                             IWirelessClock& clock_in,
                                             WirelessRuntimeConfig cfg_in)
    : link(link_in), clock(clock_in), cfg(cfg_in)
{
}

LianLiWirelessService::~LianLiWirelessService()
{
    Stop();
}

bool LianLiWirelessService::AddGroup(const Mac& mac, uint8_t fan_count)
{
    std::lock_guard<std::mutex> lock(mutex);
    if(runtimes.count(mac) != 0)
    {
        return false;
    }
    auto rt = std::make_unique<WirelessRuntime>(link, clock, cfg);
    rt->SetTarget(mac, fan_count);
    runtimes[mac] = std::move(rt);
    return true;
}

void LianLiWirelessService::RemoveGroup(const Mac& mac)
{
    std::lock_guard<std::mutex> lock(mutex);
    auto it = runtimes.find(mac);
    if(it != runtimes.end())
    {
        it->second->Cancel();
        runtimes.erase(it);
    }
}

std::vector<Mac> LianLiWirelessService::Groups() const
{
    std::lock_guard<std::mutex> lock(mutex);
    std::vector<Mac> out;
    out.reserve(runtimes.size());
    for(const auto& kv : runtimes)
    {
        out.push_back(kv.first);
    }
    return out;
}

void LianLiWirelessService::SetDesired(const Mac& mac,
                                       std::shared_ptr<const RgbUpload> upload)
{
    std::lock_guard<std::mutex> lock(mutex);
    auto it = runtimes.find(mac);
    if(it != runtimes.end())
    {
        it->second->SetDesired(std::move(upload));
    }
}

void LianLiWirelessService::ClearDesired(const Mac& mac)
{
    std::lock_guard<std::mutex> lock(mutex);
    auto it = runtimes.find(mac);
    if(it != runtimes.end())
    {
        it->second->ClearDesired();
    }
}

bool LianLiWirelessService::GetStatus(const Mac& mac, GroupStatus& out) const
{
    std::lock_guard<std::mutex> lock(mutex);
    auto it = runtimes.find(mac);
    if(it == runtimes.end())
    {
        return false;
    }
    WirelessRuntime& rt = *it->second;
    out.state          = rt.State();
    out.confirmed      = rt.Confirmed();
    out.resends        = rt.ResendCount();
    out.send_failures  = rt.SendFailures();
    out.last_error     = rt.LastError();
    const Sighting* s  = rt.Latest();
    out.have_sighting  = (s != nullptr);
    if(s != nullptr)
    {
        out.latest = *s;
    }
    return true;
}

void LianLiWirelessService::StepAll()
{
    std::lock_guard<std::mutex> lock(mutex);
    for(auto& kv : runtimes)
    {
        kv.second->Tick();
    }
}

void LianLiWirelessService::Start(uint32_t tick_ms)
{
    if(worker.joinable())
    {
        return;
    }
    stop = false;
    worker = std::thread([this, tick_ms]
    {
        while(!stop.load())
        {
            StepAll();
            std::this_thread::sleep_for(std::chrono::milliseconds(tick_ms));
        }
    });
}

void LianLiWirelessService::Stop()
{
    stop = true;
    if(worker.joinable())
    {
        worker.join();
    }
    std::lock_guard<std::mutex> lock(mutex);
    for(auto& kv : runtimes)
    {
        kv.second->Cancel();
    }
}
