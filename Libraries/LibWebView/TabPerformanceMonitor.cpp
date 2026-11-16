/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/HashTable.h>
#include <AK/SaturatingMath.h>
#include <LibRequests/RequestControlClient.h>
#include <LibWebView/Application.h>
#include <LibWebView/CanonicalNavigable.h>
#include <LibWebView/ProcessManager.h>
#include <LibWebView/RequestServerManager.h>
#include <LibWebView/TabPerformanceMonitor.h>
#include <LibWebView/ViewImplementation.h>
#include <LibWebView/WebContentClient.h>
#include <LibWebView/WebContentPage.h>

namespace WebView {

static TabPerformanceMonitor* s_monitor;

TabPerformanceMonitor& TabPerformanceMonitor::the()
{
    if (!s_monitor)
        s_monitor = new TabPerformanceMonitor;
    return *s_monitor;
}

void TabPerformanceMonitor::configure(Requests::RequestControlClient& requests)
{
    if (!s_monitor)
        return;
    requests.async_set_performance_monitor_enabled(s_monitor->m_enabled);
    if (!s_monitor->m_enabled) {
        requests.on_network_usage = nullptr;
        return;
    }
    requests.on_network_usage = [](Vector<Requests::NetworkUsage> usage, u64 interval_microseconds) {
        if (!s_monitor || !s_monitor->m_enabled)
            return;
        auto now = MonotonicTime::now();
        s_monitor->m_process_network_snapshots.clear();
        for (auto const& entry : usage) {
            if (interval_microseconds != 0) {
                auto& process_usage = s_monitor->m_process_network_snapshots.ensure(entry.process_id);
                process_usage.download_bytes_per_second = saturating_add(process_usage.download_bytes_per_second, saturating_mul(entry.download_bytes, 1'000'000ULL) / interval_microseconds);
                process_usage.upload_bytes_per_second = saturating_add(process_usage.upload_bytes_per_second, saturating_mul(entry.upload_bytes, 1'000'000ULL) / interval_microseconds);
            }
            Optional<u64> owner;
            WebContentClient::for_each_client([&](WebContentClient& client) {
                if (client.pid() != entry.process_id)
                    return IterationDecision::Continue;
                // NB: What a worker on a thread of the process uses comes with no page.
                if (auto* page = client.page(entry.page_id)) {
                    auto const& view = page->view();
                    owner = view.owner_view_id().value_or(view.view_id());
                } else {
                    owner = client.exclusive_performance_owner();
                }
                return IterationDecision::Break;
            });
            if (owner.has_value())
                s_monitor->m_tabs.ensure(*owner).add_network_bytes(now, interval_microseconds, entry.download_bytes, entry.upload_bytes);
        }
    };
}

bool TabPerformanceMonitor::process_network_statistics_enabled()
{
    return s_monitor && s_monitor->m_enabled;
}

void TabPerformanceMonitor::set_memory_pressure(bool pressured)
{
    auto& monitor = the();
    if (monitor.m_memory_pressure == pressured)
        return;
    monitor.m_memory_pressure = pressured;
    monitor.sample();
}

Optional<ProcessNetworkUsage> TabPerformanceMonitor::process_network_usage(pid_t pid)
{
    if (!s_monitor || !s_monitor->m_enabled)
        return {};
    return s_monitor->m_process_network_snapshots.get(pid);
}

void TabPerformanceMonitor::forget_view(u64 view_id)
{
    if (s_monitor) {
        s_monitor->m_tabs.remove(view_id);
        s_monitor->m_explicitly_enabled_views.remove(view_id);
        s_monitor->m_residency_enabled_views.remove(view_id);
    }
}

void TabPerformanceMonitor::set_view_enabled(u64 view_id, bool enabled)
{
    if (!s_monitor)
        return;
    if (enabled)
        s_monitor->m_explicitly_enabled_views.set(view_id);
    else
        s_monitor->m_explicitly_enabled_views.remove(view_id);
    s_monitor->sample();
}

void TabPerformanceMonitor::set_view_residency_enabled(u64 view_id, bool enabled)
{
    if (!s_monitor)
        return;
    if (enabled)
        s_monitor->m_residency_enabled_views.set(view_id);
    else
        s_monitor->m_residency_enabled_views.remove(view_id);
}

bool TabPerformanceMonitor::set_view_task_queue_diagnostics_enabled(u64, bool)
{
    // The upstream worker and WebContent process APIs no longer expose the Photon-specific
    // task queue diagnostics endpoint. Keep the embedder API fail-closed until it is adapted.
    return false;
}

String TabPerformanceMonitor::task_queue_diagnostics_json(u64)
{
    return "{\"schema_version\":1,\"scope\":\"webcontent_processes\",\"processes\":[]}"_string;
}

TabPerformanceMonitor::TabPerformanceMonitor()
{
    config_variable_changed(ConfigVariableID::ShowTabPerformanceMonitor);
}

void TabPerformanceMonitor::config_variable_changed(ConfigVariableID id)
{
    if (id != ConfigVariableID::ShowTabPerformanceMonitor)
        return;
    m_enabled = Application::settings().config_variable_as_bool(id);
    if (!m_enabled) {
        m_timer = nullptr;
        m_tabs.clear();
    }
    Application::request_server_manager().for_each_instance([](RequestServerInstance& instance) {
        configure(instance.control_client());
        return IterationDecision::Continue;
    });
    if (!m_enabled)
        return;
    m_timer = Core::Timer::create_repeating(AK::Duration::from_milliseconds(500), [this] { sample(); });
    m_timer->start();
    sample();
}

void TabPerformanceMonitor::did_present(u64 view_id)
{
    if (s_monitor && s_monitor->m_enabled)
        s_monitor->m_tabs.ensure(view_id).did_present(MonotonicTime::now());
}

void TabPerformanceMonitor::sample()
{
    HashMap<u64, HashMap<pid_t, Core::Platform::ProcessResourceUsage>> processes;
    HashMap<u64, HashTable<u64>> tabs_sharing_processes;
    Application::process_manager().for_each_process([&](Process& process) {
        if (process.type() != ProcessType::WebContent)
            return;
        auto client = process.client<WebContentClient>();
        if (!client)
            return;
        auto owners = client->performance_owners();
        if (owners.is_empty())
            return;
        auto usage = Application::process_manager().resource_usage(process.pid());
        if (!usage.has_value())
            return;
        // NB: A process that several tabs share counts in full toward each of them. Each tab then reports how
        //     many other tabs share its processes, so the UI can show that the numbers overlap.
        for (auto owner : owners) {
            processes.ensure(owner).set(process.pid(), *usage);
            for (auto other_owner : owners) {
                if (other_owner != owner)
                    tabs_sharing_processes.ensure(owner).set(other_owner);
            }
        }
    });
    auto now = MonotonicTime::now();
    ViewImplementation::for_each_view([&](ViewImplementation& view) {
        if (view.owner_view_id().has_value())
            return IterationDecision::Continue;
        Optional<pid_t> top_level_process;
        if (auto page = view.traversable().display_page())
            top_level_process = page->client().pid();
        auto stats = m_tabs.ensure(view.view_id()).sample(now, processes.ensure(view.view_id()), top_level_process);
        if (auto sharing = tabs_sharing_processes.get(view.view_id()); sharing.has_value())
            stats.other_tabs_sharing_processes = sharing->size();
        if (view.on_performance_stats)
            view.on_performance_stats(stats);
        return IterationDecision::Continue;
    });
}

}
